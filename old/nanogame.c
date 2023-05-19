#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <endian.h>

#define PORT 6969
#define BUFFER (16*1024)
#define LIMIT 64

struct packet {
	int32_t capacity;
	int32_t length;
	uint8_t data[BUFFER];
	uint8_t state;

	double x, y, z;
	bool grounded;
};

struct packet packets[LIMIT];
struct pollfd pfds[3 + LIMIT];
int connections = 0;
uint64_t tick = 0;

struct packet_buffer {
	uint8_t data[5 + BUFFER];
	uint32_t length;
	uint32_t wi;
	uint32_t ri;
};

struct player {
	struct packet_buffer *packet;

	int index, fd;
	uint8_t state;

	double x, y, z;
	bool grounded;
};

struct player players[LIMIT];

int varint_size(int32_t value)
{
	return (32 - __builtin_clz(value)) / 7 + 1;
}

struct packet_buffer *new_packet()
{
	return (struct packet_buffer*) calloc(1, sizeof(struct packet_buffer));
}

void reset_packet(struct packet_buffer *packet)
{
	packet->wi = 5;
	packet->ri = 5;
	packet->length = 0;
}

void append_byte(struct packet_buffer *packet, uint8_t value)
{
	packet->data[packet->wi++] = value;
}

uint8_t consume_byte(struct packet_buffer *packet)
{
	return packet->data[packet->ri++];
}

void append_long(struct packet_buffer *packet, uint64_t value)
{
	((uint64_t*) &packet->data[5 + packet->length])[0] = htobe64(value);
	packet->length += 8;
}

void append_varint(struct packet_buffer *packet, uint32_t value)
{
	if (!value) {
		append_byte(packet, 0);
		return;
	}

	while (value) {
		uint8_t byte = value & 0x7F;
		value >>= 7;

		if (value) {
			byte |= 0x80;
		}

		append_byte(packet, byte);
	}
}

void write_varint(uint8_t *buffer, uint32_t value)
{
	while (value) {
		*buffer |= value & 0x7F;
		value >>= 7;

		if (value) {
			*buffer |= 0b10000000;
		}

		++buffer;
	}
}

void send_packet(int fd, struct packet_buffer *packet)
{
	int length_bytes = varint_size(packet->length);
	uint8_t *start = packet->data + 5 - length_bytes;
	write_varint(start, packet->length);

	for (int i = 0; i < length_bytes + packet->length; ++i) {
		printf("%d ", start[i]);
	}

	printf("\n");

	write(fd, start, length_bytes + packet->length);
}

void print_packet(struct packet *pkt)
{
	printf("PACKET (%d/%d) 0x%02X [", pkt->length, pkt->capacity, pkt->data[0]);

	for (int i = 0; i < pkt->capacity - 1; ++i) {
		printf("%d ", pkt->data[i]);
	}

	printf("%d]\n", pkt->data[pkt->capacity - 1]);
}

void close_connection(struct player player)
{
	close(player.fd);
	free(player.packet);

	printf("closed player %d fd %d\n", player.index, player.fd);

	if (connections != 1) {
		pfds[player.index] = pfds[3 + connections - 1];
		players[player.index] = players[connections - 1];
		players[player.index].index = player.index;
	}

	--connections;
}

uint64_t to_position(int x, int y, int z)
{
	return ((uint64_t) x << 38) | ((uint64_t) y << 26) | (uint64_t) z;
}

void write_int(uint8_t *buffer, uint32_t value)
{
	*((uint32_t*) buffer) = htobe32(value);
}

void write_long(uint8_t *buffer, uint64_t value)
{
	*((uint64_t*) buffer) = htobe64(value);
}

void write_empty_chunk(int fd, int x, int z)
{
	uint8_t buffer[14] = "\x0D\x21\x00\x00\x00\x00\x00\x00\x00\x00\x01\xFF\xFF\x00";
	write_int(buffer + 2, x);
	write_int(buffer + 6, z);
	write(fd, buffer, sizeof(buffer));
}

void write_varint_fixed(uint8_t *buffer, int32_t value)
{
	for (int i = 0; i < 2; ++i) {
		buffer[i] = 0x80 | (value & 0x7F);
		value >>= 7;
	}

	buffer[2] = value & 0x7F;
}

double parse_double(uint8_t *buffer)
{
	uint64_t swapped = be64toh(*((uint64_t*) buffer));
	return *((double*) &swapped);
}

int32_t parse_varint(uint8_t *buffer)
{
	uint32_t result = 0;

	for (int shift = 0; true; shift += 7) {
		uint8_t byte = *buffer;

		result |= (byte & 0x7F) << shift;

		if (byte & 0x80 == 0) {
			break;
		}
	}

	return (int32_t) result;
}

void write_block_change(struct player *player, int x, int y, int z, uint8_t id, uint8_t meta)
{
	uint64_t position = to_position(x, y, z);
	int type = (id << 4) | meta;

	struct packet_buffer *packet = player->packet;
	reset_packet(packet);
	append_byte(packet, 0x23);
	append_long(packet, position);
	append_varint(packet, type);

	send_packet(player->fd, packet);
}

uint8_t state[16][16];


int main(int argc, char *argv[])
{
	int server = socket(AF_INET, SOCK_STREAM, 0);

	if (server == -1) {
		printf("socket creation failed\n");
		return 1;
	}

	int optval = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(PORT);

	if (bind(server, (struct sockaddr*) &address, sizeof(address)) == -1) {
		printf("failed to bind\n");
		return 1;
	}

	if (listen(server, 5) == -1) {
		printf("failed to listen\n");
		return 1;
	}

	printf("listening\n");

	pfds[0].fd = 0;
	pfds[0].events = POLLIN;

	pfds[1].fd = server;
	pfds[1].events = POLLIN;

	pfds[2].fd = timerfd_create(CLOCK_REALTIME, 0);
	pfds[2].events = POLLIN;

	struct itimerspec spec = {
		{ 0, 50000000 },
		{ 0, 50000000 }
	};

	timerfd_settime(pfds[2].fd, 0, &spec, NULL);

	while (true) {
		poll(pfds, 3 + connections, -1);

		if (pfds[0].revents & POLLIN) {
			uint8_t buf[1];
			read(pfds[0].fd, buf, sizeof(buf));
			printf("AYE %d\n", buf[0]);
		}

		if (pfds[1].revents & POLLIN) {
			int fd = accept4(server, NULL, NULL, /*O_NONBLOCK*/0);

			if (connections < LIMIT) {
				pfds[3 + connections].fd = fd;
				pfds[3 + connections].events = POLLIN;

				players[connections].packet = new_packet();
				players[connections].index = 3 + connections;
				players[connections].fd = fd;
				players[connections].state = 0;

				++connections;

				printf("accepted connection (%d/%d) %d\n", connections, LIMIT, fd);
			} else {
				printf("connection dropped\n");
				close(fd);
			}
		}

		if (pfds[2].revents & POLLIN) {
			uint64_t whatever;
			read(pfds[2].fd, &whatever, 8);

			if (tick % 200 == 0) {
				for (int i = 0; i < connections; ++i) {
					write(players[i].fd, "\x00\x00\x00", 3);
				}

				if (connections) {
					printf("sent keepalives\n");
				}
			}

			++tick;
		}

		for (int i = 0; i < connections; ++i) {
			struct player *player = &players[i];
			struct packet_buffer *packet = player->packet;

			if (pfds[3 + i].revents & POLLIN) {
				int n;

				if (packet->length == 0) {
					uint8_t byte;
					n = read(player->fd, &byte, 1);

					if (n == 1) {
						packet->data[packet->wi++] = byte;

						if (byte & 0x80 == 0) {
							packet->length = parse_varint(player.packet->data);
							packet->wi = 5 + packet->length;
							packet->ri = 5;
						}
					}
				} else {
					n = read(player.fd, packet->data + packet->wi, packet->length - packet->wi);

					if (n >= 1) {
						packet->wi += n;
					}
				}

				if (n <= 0) {
					printf("CLOSE %d\n", __LINE__);
					close_connection(player);
					break;
				}

				if (packet->wi == packet->length) {
					uint8_t id = consume_byte(packet);

					if (player.state == 0) {
						if (id == 0x00) {
							uint8_t next_state = packet->data[packet->length - 1];
							printf("HANDSHAKE %d\n", next_state);

							if (next_state == 1) {
								player.state = 1;
							} else if (next_state == 2) {
								player.state = 2;
							}
						}
					} else if (player->state == 1) {
						if (id == 0x00) {
							char message[] = "\x6F" "\x00" "\x6D" "{version:{name:'1.8.8',protocol:47},players:{max:420,online: 0},description:{text:'color shit',color:'aqua'}}";
							message[63] = (connections >= 10) ? (connections / 10 + 48) : ' ';
							message[64] = connections % 10 + 48;
							write(player.fd, message, 112);
						} else if (id == 0x01) {
							//write(player.fd, "\x09", 1);
							//write(player.fd, packet->data, packet->length);							
							//close_connection(i);
						}
					} else if (player->state == 2) {
						if (id == 0x00) {
							printf("LOGIN START\n");

							uint8_t name_length = packet->data[1];
							printf("name %.*s\n", name_length, packet->data + 2);

							write(player.fd, "\x37" "\x02" "\x24" "750fc259-3bb1-44db-97bd-2e995e0089b4" "\x10" "globglogabgalab3", 56);
							write(player.fd, "\x12" "\x01" "\x00\x00\x00\x01" "\x01" "\x00" "\x00" "\x20" "\x07" "default" "\x00", 19);
							write(player.fd, "\x09" "\x05" "\x00\x00\x10\x00\x00\x00\x00\x00", 10);
							write(player.fd, "\x22" "\x08" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00" "\x00\x00\x00\x00" "\xFF", 35);
							write_empty_chunk(player->fd, 0, 0);

							for (int x = 0; x < 16; ++x) {
								for (int z = 0; z < 16; ++z) {
									write_block_change(player->fd, x, 60, z, 159, state[x][z]);
								}
							}

							player.state = 3;
						}
					} else if (player->state == 3) {
						if (id == 0x01) {
							// TODO: kick if length too big
							uint8_t length = packet->data[1] & 0x7F;

							char *prefix = "[{text:'player: ',color:'aqua'},{text:'";
							char *suffix = "',color:'white'}]";

							uint8_t buffer[3 + 1 + 3 + strlen(prefix) + length + strlen(suffix) + 1];
							write_varint_fixed(buffer, 1 + 3 + strlen(prefix) + length + strlen(suffix) + 1);
							buffer[3] = 0x02;
							write_varint_fixed(buffer + 3 + 1, strlen(prefix) + length + strlen(suffix));
							memcpy(buffer + 3 + 1 + 3, prefix, strlen(prefix));
							memcpy(buffer + 3 + 1 + 3 + strlen(prefix), packet->data + 2, length);
							memcpy(buffer + 3 + 1 + 3 + strlen(prefix) + length, suffix, strlen(suffix));
							buffer[3 + 1 + 3 + strlen(prefix) + length + strlen(suffix)] = 0;

							for (int j = 0; j < connections; ++j) {
								write(pfds[3 + j].fd, buffer, sizeof(buffer));
							}
						} else if (id == 0x03) {
							packet->grounded = packet->data[2];							
						} else if (id == 0x04 || id == 0x06) {
							player->x = parse_double(packet->data + 1);
							player->y = parse_double(packet->data + 9);
							player->z = parse_double(packet->data + 17);

							if (id == 0x04) {
								packet->grounded = packet->data[25];
							} else if (id == 0x06) {
								packet->grounded = packet->data[33];
							}

							printf("move to %f %f %f %d\n", player->x, player->y, player->z, player->grounded);

							int ax = player->x;
							int az = player->z;

							int new = client % 14 + 1;

							if (packet.grounded && ax >= 0 && ax < 16 && az >= 0 && az < 16 && state[ax][az] != new) {
								state[ax][az] = new;
								printf("WA %d\n", client % 14 + 1);
								write_block_change(player, ax, 60, az, 0, 0);
								write_block_change(player, ax, 60, az, 159, state[ax][az]);
							}
						}
					}

					packet->wi = 0;
					packet->length = 0;
				}
			}
		}
	}

	close(server);

	return 0;
}
