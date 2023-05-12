#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <endian.h>

#define PORT 6969
#define BUFFER (16*1024)
#define LIMIT 64

struct packet {
	int32_t capacity;
	int32_t length;
	uint8_t data[BUFFER];
	uint8_t state;
};

struct packet packets[LIMIT];
struct pollfd pfds[2 + LIMIT];
int clients = 0;

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
	int fd = pfds[2 + i].fd;

	close(fd);
	printf("closed index %d fd %d\n", i, fd);

	if (clients != 1) {
		pfds[i] = pfds[2 + clients - 1];
		packets[i] = packets[clients - 1];
	}

	--clients;
}

void write_varint(uint8_t **cursor, int32_t value)
{
	while (value) {
		**cursor = value & 0x7F;
		value >>= 7;

		if (value) {
			**cursor |= 0x80;
		}

		++*cursor;
	}
}

void write_string(uint8_t **cursor, char *value)
{
	write_varint(cursor, strlen(value));

	for (char *c = value; *c; ++c) {
		**cursor = *c;
		++*cursor;
	}
}

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

	while (true) {
		poll(pfds, clients + 2, -1);

		if (pfds[0].revents & POLLIN) {
			uint8_t buf[1];
			read(pfds[0].fd, buf, sizeof(buf));
			printf("AYE %d\n", buf[0]);
		}

		if (pfds[1].revents & POLLIN) {
			int client = accept(server, NULL, NULL);

			if (clients < LIMIT) {
				pfds[2 + clients].fd = client;
				pfds[2 + clients].events = POLLIN;
				packets[clients].capacity = 0;
				packets[clients].length = -1;
				packets[clients].state = 0;

				++clients;

				printf("accepted connection (%d/%d)\n", clients, LIMIT);
			} else {
				printf("connection dropped\n");
				close(client);
			}
		}

		for (int i = 0; i < clients; ++i) {
			int client = pfds[2 + i].fd;

			if (pfds[2 + i].revents & POLLIN) {
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
					close_connection(i);
					break;
				}

				if (packets[i].length == packets[i].capacity) {
					print_packet(&packets[i]);

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
							write(client, "\x5B" "\x00" "\x59" "{version:{name:'1.8.8',protocol:47},players:{max:420,online:69},description:{text:'ass'}}", 92);
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
							write(client, "\x0D" "\x21" "\x00\x00\x00\x00" "\x00\x00\x00\x00" "\x01" "\xFF\xFF" "\x00", 14);

							{
								uint8_t buffer[11];
								buffer[0] = 10;
								buffer[1] = 0x23;

								int y = 60;

								buffer[10] = 1 << 4;

								for (int x = 0; x < 16; ++x) {
									for (int z = 0; z < 16; ++z) {
										uint64_t position = ((uint64_t) x << 38) | ((uint64_t) y << 26) | (uint64_t) z;
										*((uint64_t*) (buffer + 2)) = htobe64(position);

										write(client, buffer, sizeof(buffer));
									}
								}
							}

							packets[i].state = 3;
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
