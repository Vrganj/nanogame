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
int clients = 0;
uint64_t tick = 0;

void print_packet(struct packet *pkt)
{
	printf("PACKET (%d/%d) 0x%02X [", pkt->length, pkt->capacity, pkt->data[0]);

	for (int i = 0; i < pkt->capacity - 1; ++i) {
		printf("%d ", pkt->data[i]);
	}

	printf("%d]\n", pkt->data[pkt->capacity - 1]);
}

void close_connection(int i)
{
	int fd = pfds[3 + i].fd;

	close(fd);
	printf("closed index %d fd %d\n", i, fd);

	if (clients != 1) {
		pfds[i] = pfds[3 + clients - 1];
		packets[i] = packets[clients - 1];
	}

	--clients;
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

void write_block_change(int fd, int x, int y, int z, int id, int meta)
{
	uint64_t position = to_position(x, y, z);
	int type = (id << 4) | meta;

	if (type < 128) {
		uint8_t buffer[11];
		buffer[0] = 10;
		buffer[1] = 0x23;
		write_long(buffer + 2, position);
		buffer[10] = type;
		write(fd, buffer, sizeof(buffer));
	} else {
		uint8_t buffer[12];
		buffer[0] = 11;
		buffer[1] = 0x23;
		write_long(buffer + 2, position);
		buffer[10] = 0x80 | (type & 0xFF);
		buffer[11] = type >> 7;
		write(fd, buffer, sizeof(buffer));
	}
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

int varint_size(int32_t value)
{
	return __builtin_clz(value) / 7 + 1;
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
		poll(pfds, clients + 3, -1);

		if (pfds[0].revents & POLLIN) {
			uint8_t buf[1];
			read(pfds[0].fd, buf, sizeof(buf));
			printf("AYE %d\n", buf[0]);
		}

		if (pfds[1].revents & POLLIN) {
			int client = accept4(server, NULL, NULL, /*O_NONBLOCK*/0);

			if (clients < LIMIT) {
				pfds[3 + clients].fd = client;
				pfds[3 + clients].events = POLLIN;
				packets[clients].capacity = 0;
				packets[clients].length = -1;
				packets[clients].state = 0;

				++clients;

				printf("accepted connection (%d/%d) %d\n", clients, LIMIT, client);
			} else {
				printf("connection dropped\n");
				close(client);
			}
		}

		if (pfds[2].revents & POLLIN) {
			uint64_t whatever;
			read(pfds[2].fd, &whatever, 8);

			if (tick % 200 == 0) {
				for (int i = 0; i < clients; ++i) {
					int fd = pfds[3 + i].fd;
					write(fd, "\x00\x00\x00", 3);
				}

				if (clients) {
					printf("sent keepalives\n");
				}
			}

			++tick;
		}

		for (int i = 0; i < clients; ++i) {
			int client = pfds[3 + i].fd;

			if (pfds[3 + i].revents & POLLIN) {
				int n;

				if (packets[i].length < 0) {

					uint8_t byte;
					n = read(client, &byte, 1);

					if (n == 1) {
						packets[i].capacity |= (byte & 0b01111111) << (-7 * packets[i].length - 7);
						--packets[i].length;

						if ((byte & 0b10000000) == 0) {
							packets[i].length = 0;
						}
					}
				} else {
					n = read(client, packets[i].data + packets[i].length, packets[i].capacity - packets[i].length);

					if (n >= 1) {
						packets[i].length += n;
					}
				}

				if (n <= 0) {
					printf("CLOSE %d\n", __LINE__);
					close_connection(i);
					break;
				}

				if (packets[i].length == packets[i].capacity) {
					uint8_t id = packets[i].data[0];

					if (packets[i].state == 0) {
						if (id == 0x00) {
							uint8_t next_state = packets[i].data[packets[i].capacity - 1];
							printf("HANDSHAKE %d\n", next_state);

							if (next_state == 1) {
								packets[i].state = 1;							
							} else if (next_state == 2) {
								packets[i].state = 2;
							}
						}
					} else if (packets[i].state == 1) {
						if (id == 0x00) {
							char message[] = "\x6F" "\x00" "\x6D" "{version:{name:'1.8.8',protocol:47},players:{max:420,online: 0},description:{text:'color shit',color:'aqua'}}";
							message[63] = (clients >= 10) ? (clients / 10 + 48) : ' ';
							message[64] = clients % 10 + 48;
							write(client, message, 112);
						} else if (id == 0x01) {
							write(client, "\x09", 1);
							write(client, packets[i].data, packets[i].length);							
							close_connection(i);
						}
					} else if (packets[i].state == 2) {
						if (id == 0x00) {
							printf("LOGIN START\n");

							uint8_t name_length = packets[i].data[1];
							printf("name %.*s\n", name_length, &packets[i].data[2]);

							write(client, "\x37" "\x02" "\x24" "750fc259-3bb1-44db-97bd-2e995e0089b4" "\x10" "globglogabgalab3", 56);
							write(client, "\x12" "\x01" "\x00\x00\x00\x01" "\x01" "\x00" "\x00" "\x20" "\x07" "default" "\x00", 19);
							write(client, "\x09" "\x05" "\x00\x00\x10\x00\x00\x00\x00\x00", 10);
							write(client, "\x22" "\x08" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00" "\x00\x00\x00\x00" "\xFF", 35);
							write_empty_chunk(client, 0, 0);

							for (int x = 0; x < 16; ++x) {
								for (int z = 0; z < 16; ++z) {
									write_block_change(client, x, 60, z, 159, state[x][z]);
								}
							}

							packets[i].state = 3;
						}
					} else if (packets[i].state == 3) {
						if (id == 0x01) {
							// TODO: kick if length too big
							uint8_t length = packets[i].data[1] & 0x7F;

							char *prefix = "[{\"text\":\"Player: \",\"color\":\"aqua\"},{\"text\":\"";
							char *suffix = "\",\"color\":\"white\"}]";

							uint8_t buffer[3 + 1 + 3 + strlen(prefix) + length + strlen(suffix) + 1];
							write_varint_fixed(buffer, 1 + 3 + strlen(prefix) + length + strlen(suffix) + 1);
							buffer[3] = 0x02;
							write_varint_fixed(buffer + 3 + 1, strlen(prefix) + length + strlen(suffix));
							memcpy(buffer + 3 + 1 + 3, prefix, strlen(prefix));
							memcpy(buffer + 3 + 1 + 3 + strlen(prefix), packets[i].data + 2, length);
							memcpy(buffer + 3 + 1 + 3 + strlen(prefix) + length, suffix, strlen(suffix));
							buffer[3 + 1 + 3 + strlen(prefix) + length + strlen(suffix)] = 0;

							for (int j = 0; j < clients; ++j) {
								write(pfds[3 + j].fd, buffer, sizeof(buffer));
							}
						} else if (id == 0x03) {
							packets[i].grounded = packets[i].data[2];							
						} else if (id == 0x04 || id == 0x06) {
							packets[i].x = parse_double(packets[i].data + 1);
							packets[i].y = parse_double(packets[i].data + 9);
							packets[i].z = parse_double(packets[i].data + 17);

							if (id == 0x04) {
								packets[i].grounded = packets[i].data[25];
							} else if (id == 0x06) {
								packets[i].grounded = packets[i].data[33];
							}

							printf("move to %f %f %f %d\n", packets[i].x, packets[i].y, packets[i].z, packets[i].grounded);

							int ax = packets[i].x;
							int az = packets[i].z;

							int new = client % 14 + 1;

							if (packets[i].grounded && ax >= 0 && ax < 16 && az >= 0 && az < 16 && state[ax][az] != new) {
								state[ax][az] = new;
								printf("WA %d\n", client % 14 + 1);
								write_block_change(client, ax, 60, az, 0, 0);
								write_block_change(client, ax, 60, az, 159, state[ax][az]);
							}
						}
					}

					packets[i].length = -1;
					packets[i].capacity = 0;
				}
			}
		}
	}

	close(server);

	return 0;
}
