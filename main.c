#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <zlib.h>
#include <arpa/inet.h>
#include <stdbool.h>

#define PORT 25565
#define MAX_PLAYERS 32
#define MAP_VOLUME (256 * 256 *64)

#define COLOR_BLACK     '0'
#define COLOR_NAVY      '1'
#define COLOR_GREEN     '2'
#define COLOR_TEAL      '3'
#define COLOR_MAROON    '4'
#define COLOR_PURPLE    '5'
#define COLOR_GOLD      '6'
#define COLOR_SILVER    '7'
#define COLOR_GRAY      '8'
#define COLOR_BLUE      '9'
#define COLOR_LIME      'a'
#define COLOR_AQUA      'b'
#define COLOR_RED       'c'
#define COLOR_PINK      'd'
#define COLOR_YELLOW    'e'
#define COLOR_WHITE     'f'


typedef struct {
    uv_tcp_t handle;
    uint8_t buffer[1024]; 
    size_t buffer_len;
} client_context_t;

typedef struct {
    int active;
    client_context_t* net_context;
    char username[64];
    uint16_t x, y, z;
    uint8_t yaw, pitch;
} Player;

typedef struct {
    uint8_t* world_data_compressed;
    long world_data_compressed_size;
    uint8_t* world_data;
    int16_t spawn_x, spawn_y, spawn_z;
} World;

#pragma pack(push,1)
typedef struct {
    uint8_t packet_id; 
    uint8_t protocol_version; 
    char username[64]; 
    char verification_key[64];
    uint8_t unused;
} PacketPlayerIdent;


typedef struct {
    uint8_t packet_id; 
    uint8_t protocol_version; 
    char server_name[64];
    char server_motd[64];
    uint8_t user_type; 
} PacketServerIdent;

typedef struct {
    uint8_t packet_id;
    short x,y,z;
    uint8_t mode;
    uint8_t block_type; 
} PacketSetBlock;

typedef struct {
    uint8_t packet_id;
} PacketLevelInitialize;

typedef struct {
    uint8_t packet_id;
    unsigned short chunk_length;
    uint8_t chunk_data[1024]; // 1024 bytes, int = 4 bytes
    uint8_t percent_complete;
} PacketLevelDataChunk;

typedef struct {
    uint8_t packet_id;
    uint16_t x_size,y_size,z_size;
} PacketLevelFinalize;

typedef struct {
    uint8_t packet_id;
    int8_t player_id;
    char player_name[64];
    uint16_t x,y,z;
    uint8_t yaw, pitch;
} PacketSpawnPlayer;

typedef struct {
    uint8_t packet_id;
    uint8_t player_id;
    uint16_t x,y,z;
    uint8_t yaw, pitch;
} PacketPositionAndOrientation; // Player move

typedef struct {
    uint8_t packet_id;
    int8_t player_id;
} PacketDespawnPlayer;

typedef struct {
    uint8_t packet_id;
    int8_t player_id;
    char message[64];
} PacketMessage;

typedef struct {
    uint8_t packet_id;
    char disconnect_reason[64];
} PacketDisconnectPlayer;

typedef struct {
    uint8_t packet_id;
    uint8_t user_type;
} PacketUpdateUserType;

#pragma pack(pop)

Player players[MAX_PLAYERS];
World world;

void load_map(const char* filename) {
    // 1. Open the gzipped file using zlib's native stream function
    gzFile file = gzopen(filename, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: Could not open gzip file %s\n", filename);
        return;
    }

    // 2. Read and skip the first 344 bytes (The Java Serialization header)
    uint8_t garbage_header[344];
    int bytes_read = gzread(file, garbage_header, 344);
    if (bytes_read < 344) {
        fprintf(stderr, "Error: server_level.dat is corrupted or too small.\n");
        gzclose(file);
        return;
    }

    // Read spawn location
    long spawn_location[3];
    memcpy(spawn_location, garbage_header + 284, 12);
    world.spawn_x = htons(ntohl(spawn_location[0])*32);
    world.spawn_y = htons(ntohl(spawn_location[2])*32); // Y location is incorrect, needs to be inferred from world or otherwise set.
    world.spawn_z = htons(ntohl(spawn_location[1])*32);


    // 3. Allocate a buffer large enough for our network payload:
    // [ 4-byte big-endian volume length ] + [ 4,194,304 bytes of blocks ]
    size_t network_payload_size = MAP_VOLUME + 4;
    uint8_t* network_payload = malloc(network_payload_size);
    if (!network_payload) {
        perror("Out of memory allocating network payload");
        gzclose(file);
        return;
    }

    // 4. Inject the Big-Endian volume frame header the client requires
    uint32_t network_volume = htonl(MAP_VOLUME);
    memcpy(network_payload, &network_volume, 4);

    // 5. Read the block array directly out of the zlib stream into the payload buffer
    uint8_t* block_destination = network_payload + 4;
    bytes_read = gzread(file, block_destination, MAP_VOLUME);
    if (bytes_read < MAP_VOLUME) {
        fprintf(stderr, "Error: Could only read %d bytes of block data (Expected %d).\n", bytes_read, MAP_VOLUME);
        free(network_payload);
        gzclose(file);
        return;
    }

    printf("Successfully extracted raw map array from server_level.dat!\n");
    gzclose(file); // Done reading the file

    // 6. Re-compress this pristine payload with a GZIP wrapper for transmission
    ulong max_compressed_size = compressBound(network_payload_size) + 18;
    uint8_t* compressed_buffer = malloc(max_compressed_size);
    if (!compressed_buffer) {
        perror("Out of memory allocating compression space");
        free(network_payload);
        return;
    }

    z_stream stream;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.avail_in = network_payload_size;
    stream.next_in = network_payload;
    stream.avail_out = max_compressed_size;
    stream.next_out = compressed_buffer;

    // Initialize with 16 + 15 window bits to force regular GZIP encapsulation
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + 15, 8, Z_DEFAULT_STRATEGY);
    deflate(&stream, Z_FINISH);
    deflateEnd(&stream);

    // 7. Store safely inside your global world tracker
    // Free previous allocations if doing a map reload while server is running
    if (world.world_data_compressed) {
        free(world.world_data_compressed);
    }
    
    world.world_data_compressed = compressed_buffer;
    world.world_data_compressed_size = stream.total_out;

    free(network_payload);
    printf("Transmittable network map buffer built! Size: %ld bytes\n", world.world_data_compressed_size);
}

void format_classic_string(char *dest, const char* src) {
    memset(dest, ' ', 64);
    size_t len = strlen(src);
    if (len > 64) len = 64;
    memcpy(dest, src, len);
}

void format_print_string(char* safe_string) {
    for (int i = 0; i < 64; i++) {
        if(safe_string[i] == ' ') {
            safe_string[i] = '\0';
            return;
        }
    }
}

void on_write(uv_write_t *req, int status) {
    if (status < 0) {
        fprintf(stderr, "Write error: %s\n", uv_strerror(status));
    }
    free(req); // Freeing the write request
}

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

void send_packet(client_context_t *ctx, size_t packet_size, void* packet) {
    uv_write_t *write_req = malloc(sizeof(uv_write_t));
    char* write_buffer = malloc(packet_size);
    memcpy(write_buffer, packet, packet_size);
    
    uv_buf_t uv_buf = uv_buf_init(write_buffer, packet_size);
    uv_write(write_req, (uv_stream_t*)&ctx->handle, &uv_buf, 1, on_write);
}

int get_player_id(client_context_t *ctx) {
    for(int i = 0; i < MAX_PLAYERS; i++) {
        if(players[i].net_context == ctx)
        return i;
    }
    // Something went wrong
    fprintf(stderr, "Player ID not found!");
    return 0;
}

void broadcast_packet_others(int player_id, size_t packet_size, void *packet) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if(i != player_id && players[i].active) {
            send_packet(players[i].net_context, packet_size, packet);
        }
    }
}

void broadcast_packet_all(size_t packet_size, void *packet) {
    for(int i = 0; i < MAX_PLAYERS; i++) {
        if(players[i].active) {            
            send_packet(players[i].net_context, packet_size, packet);
        }
    }
}

// 1. Clean up player array and free memory on disconnect
void on_client_close(uv_handle_t *handle) {
    client_context_t *ctx = (client_context_t*)handle->data;
    
    // Find and remove them from the players array
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active && players[i].net_context == ctx) {
            PacketDespawnPlayer packet;
            packet.packet_id = 0x0c;
            packet.player_id = i;
            broadcast_packet_others(i, sizeof(PacketDespawnPlayer), &packet);
            
            players[i].active = 0;
            players[i].net_context = NULL;
            
            char safe_name[65] = {0};
            memcpy(safe_name, players[i].username, 64);
            format_print_string(safe_name);
            printf("Player %s disconnected. Cleaned up slot %d.\n", safe_name, i); 
            break;
        }
    }
    
    // Crucial: Free the master context structure
    free(ctx);
}


void send_world_data(client_context_t *ctx) {
    // Send initialize packet
    PacketLevelInitialize level_initialize;
    level_initialize.packet_id = 0x02;
    send_packet(ctx, sizeof(PacketLevelInitialize), &level_initialize);

    size_t bytes_sent = 0;
    while(bytes_sent < world.world_data_compressed_size) {
        size_t remaining = world.world_data_compressed_size - bytes_sent;
        size_t chunk_size = (remaining > 1024) ? 1024 : remaining;

        PacketLevelDataChunk packet;
        packet.packet_id = 0x03;
        packet.chunk_length = htons((uint16_t)chunk_size);

        memset(packet.chunk_data, 0, 1024);
        memcpy(packet.chunk_data, world.world_data_compressed + bytes_sent, chunk_size);
        bytes_sent += chunk_size;
        packet.percent_complete = (uint8_t)((bytes_sent * 100) / world.world_data_compressed_size);

        send_packet(ctx, sizeof(PacketLevelDataChunk), &packet);
    }
    // Send finalize packet
    PacketLevelFinalize level_finalize;
    level_finalize.packet_id = 0x04;
    level_finalize.x_size = htons(256);
    level_finalize.y_size = htons(64);
    level_finalize.z_size = htons(256);
    send_packet(ctx, sizeof(PacketLevelFinalize), &level_finalize);
}

void set_player_position(client_context_t *ctx, uint8_t player_id, uint16_t x, uint16_t y, uint16_t z, uint8_t yaw, uint8_t pitch, bool spawn) {
    uint8_t pid;
    // If the player is spawning, we don't want to update the array of players.
    if(spawn) pid = 255;
    else pid = player_id;
    players[player_id].x = x;
    players[player_id].y = y;
    players[player_id].z = z;
    players[player_id].pitch = pitch;
    players[player_id].yaw = yaw;

    PacketPositionAndOrientation packet;
    packet.packet_id = 0x08;
    packet.player_id = pid;
    packet.x = x;
    packet.y = y;
    packet.z = z;
    packet.yaw = yaw;
    packet.pitch = pitch;
    send_packet(ctx, sizeof(PacketPositionAndOrientation), &packet);
}

void broadcast_player_join(int player_id) {

    for (int i = 0; i < MAX_PLAYERS; i++) {
        // Don't broadcast join to other players
        if(i == player_id) continue;
        if(players[i].active) {
            // Send to other players
            client_context_t *ctx = players[i].net_context;
            PacketSpawnPlayer packet;
            packet.packet_id = 0x07;
            packet.player_id = player_id;
            memcpy(packet.player_name, players[player_id].username,64);
            packet.x = players[player_id].x;
            packet.y = players[player_id].y;
            packet.z = players[player_id].z;
            packet.yaw = players[player_id].yaw;
            packet.pitch = players[player_id].pitch;
            send_packet(ctx, sizeof(PacketSpawnPlayer), &packet);
            
            // Send spawn to player who joined
            client_context_t *ctx_p = players[player_id].net_context;
            PacketSpawnPlayer packet_p;
            packet_p.packet_id = 0x07;
            packet_p.player_id = i;
            memcpy(packet_p.player_name, players[i].username,64);
            packet_p.x = players[i].x;
            packet_p.y = players[i].y;
            packet_p.z = players[i].z;
            packet_p.yaw = players[i].yaw;
            packet_p.pitch = players[i].pitch;
            send_packet(ctx_p, sizeof(PacketSpawnPlayer), &packet_p);
        }
    }
    // Announce player join with chat message!
    /*
    Color coding at the start of the message will only work if the 
    player ID byte is less than 127. If it's 127 or higher, 
    the game automatically adds &e before the message, making it yellow.
    */
    PacketMessage join_message;
    join_message.packet_id = 0x0d;
    join_message.player_id = -1; // Yellow text
    char username[64];
    char message_str[64];
    char *greet_message = " joined the game!";
    memcpy(username, players[player_id].username, 64);
    format_print_string(username);
    // Check username length, could cause buffer overlow otherwise
    if(strlen(username) < 64-strlen(greet_message)) {
        sprintf(message_str, "%s%s", username, greet_message);
        format_classic_string(join_message.message, message_str);
        broadcast_packet_all(sizeof(PacketMessage), &join_message);    
    }
    }

// Pass the raw packet payload rather than the context buffer
void player_connect(client_context_t *ctx, uint8_t *packet_data) {
    PacketPlayerIdent* ident = (PacketPlayerIdent*)packet_data;
    // Safely print username
    char safe_name[65] = {0};
    memcpy(safe_name, ident->username, 64);
    format_print_string(safe_name);

    for(int i = 0; i < MAX_PLAYERS; i++) {
        if(!players[i].active) {
            players[i].active = 1;
            players[i].net_context = ctx;
            memcpy(players[i].username, ident->username, 64);
            
            // Default Classic spawn coords usually float somewhere mid-air or map center

            printf("Player slot %d assigned to: %s\n", i, safe_name);

            // Send back Server Identification
            PacketServerIdent reply;
            reply.packet_id = 0x00;
            reply.protocol_version = 0x07;
            format_classic_string(reply.server_name, "C-Classic Server");
            format_classic_string(reply.server_motd, "Welcome!");
            reply.user_type = 0x00;

            // Send Server Identification packet!
            send_packet(ctx, sizeof(PacketServerIdent), &reply);
            
            send_world_data(ctx);
            // TODO: Replace with spawn coordinates fetched from file
            set_player_position(ctx, 0, world.spawn_x, world.spawn_y, world.spawn_z, 0, 0, true);
            
            // TODO: Broadcast player join to other players
            broadcast_player_join(i);
            return;
        }
    }
    // Server is full, send disconnect packet.
    printf("Player %s tried to connect but server is full!\n", safe_name);

    PacketDisconnectPlayer reply;
    reply.packet_id = 0x0e;
    format_classic_string(reply.disconnect_reason, "Server is full!");

    send_packet(ctx, sizeof(PacketDisconnectPlayer), &reply);
    uv_close((uv_handle_t*)&ctx->handle, on_client_close);
}

void client_set_block(client_context_t *ctx, uint8_t *data) {
    // TODO: Alter world array
    // TODO: Compress world each time player joins
    printf("SETBLOCK\n");
}
void client_position(client_context_t *ctx, uint8_t *data) {
    PacketPositionAndOrientation* pos = (PacketPositionAndOrientation*)data;
    uint8_t player_id = get_player_id(ctx); 
    // Broadcast position to other players
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if(players[i].active && players[i].net_context != ctx) {
            set_player_position(players[i].net_context, player_id, pos->x, pos->y, pos->z, pos->yaw, pos->pitch, false);
        }
    }
}

// Chat messages
void client_message(client_context_t *ctx, uint8_t *data) {
    PacketMessage* mes = (PacketMessage*)data;
    uint8_t player_id = get_player_id(ctx);
    // Broadcast message to other players
    PacketMessage packet;
    packet.packet_id = 0x0d;
    packet.player_id = player_id;
    // Do string formatting!
    // {username}: memcpy Message(64-head) to some string head
    // Get safe username
    char message[64];
    // Copy username to message, get username length, write ':' after username
    memcpy(message, players[player_id].username, 64);
    format_print_string(message);
    // Get length and write to the string character by character
    // Then append the message at the write head
    int len = strlen(message);
    int head = len;
    message[head++] = ':'; // Set \0 to :, increment head
    message[head++] = ' ';
    memcpy(message+head, mes->message, 64-head);

    // Copy message to packet and broadcast packet!
    memcpy(packet.message, message, 64); 
    broadcast_packet_all(sizeof(PacketMessage), &packet);
}

// Handle incoming packages
void on_read(uv_stream_t *client_stream, ssize_t nread, const uv_buf_t *buf) {
    client_context_t *ctx = (client_context_t*)client_stream->data;

    if (nread > 0) {
        // Guard against buffer overflows
        if (ctx->buffer_len + nread > sizeof(ctx->buffer)) {
            fprintf(stderr, "Client buffer overflow! Forcing disconnect.\n");
            uv_close((uv_handle_t*)client_stream, on_client_close);
            if (buf->base) free(buf->base);
            return;
        }

        memcpy(ctx->buffer + ctx->buffer_len, buf->base, nread);
        ctx->buffer_len += nread;

        while (ctx->buffer_len > 0) {
            uint8_t packet_id = ctx->buffer[0];
            size_t expected_size = 0;

            if (packet_id == 0x00) expected_size = 131; // Player Ident
            else if (packet_id == 0x05) expected_size = 9;   // Set block
            else if (packet_id == 0x08) expected_size = 10;  // Position
            else if (packet_id == 0x0d) expected_size = 66;  // Message
            else {
                fprintf(stderr, "Unknown packet 0x%02X! Disconnecting client.\n", packet_id);
                uv_close((uv_handle_t*)client_stream, on_client_close);
                break;
            }

            if (ctx->buffer_len >= expected_size) {
                // Pass a pointer directly to the individual packet slice
                uint8_t *packet_payload = ctx->buffer;
                if(packet_id == 0x00) player_connect(ctx, packet_payload);
                else if(packet_id == 0x05) client_set_block(ctx, packet_payload);
                else if(packet_id == 0x08) client_position(ctx, packet_payload);
                else if(packet_id == 0x0d) client_message(ctx, packet_payload);

                memmove(ctx->buffer, ctx->buffer + expected_size, ctx->buffer_len - expected_size);
                ctx->buffer_len -= expected_size;
            } else {
                break; // Await more bytes
            }
        }
    } else if (nread < 0) {
        // nread < 0 handled properly now!
        if (nread != UV_EOF) {
            fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
        }
        uv_close((uv_handle_t*)client_stream, on_client_close);
    }

    if (buf->base) free(buf->base);
}

void on_new_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        fprintf(stderr, "New connection error: %s\n", uv_strerror(status));
        return;
    }

    // Allocate our custom compound context struct
    client_context_t *ctx = malloc(sizeof(client_context_t));
    ctx->buffer_len = 0;

    uv_tcp_init(server->loop, &ctx->handle);
    
    // Cross-link: point the uv_handle's data back to our custom struct wrapper
    ctx->handle.data = ctx; 

    if (uv_accept(server, (uv_stream_t*)&ctx->handle) == 0) {
        uv_read_start((uv_stream_t*)&ctx->handle, alloc_buffer, on_read);
        printf("Client context assigned and loop listening...\n");
    } else {
        uv_close((uv_handle_t*)&ctx->handle, on_client_close);
    }
}

int main() {
    load_map("./osici-europe/server_level.dat");
    // Clear array memory safely at startup
    memset(players, 0, sizeof(players));

    uv_loop_t *loop = uv_default_loop();
    uv_tcp_t server;
    uv_tcp_init(loop, &server);

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", PORT, &addr);

    uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
    
    int r = uv_listen((uv_stream_t*)&server, 128, on_new_connection);
    if (r) {
        fprintf(stderr, "Listen error: %s\n", uv_strerror(r));
        return 1;
    }

    printf("Minecraft Classic server running on port %d...\n", PORT);
    return uv_run(loop, UV_RUN_DEFAULT);
}