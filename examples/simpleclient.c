// gcc -o echo_bot echo_bot.c -std=gnu99 -lsodium libtoxcore.a
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>

#include <sodium/utils.h>
#include <tox/tox.h>

Tox *tox;

TOX_CONNECTION tox_connection_status = TOX_CONNECTION_NONE;

typedef struct DHT_node {
    const char *ip;
    uint16_t port;
    const char key_hex[TOX_PUBLIC_KEY_SIZE*2 + 1];
    unsigned char key_bin[TOX_PUBLIC_KEY_SIZE];
} DHT_node;

char LineBuf[BUFSIZ];

uint32_t TalkingTo = 0;
uint8_t IsTalking = 0;

const char *REPL_PROMPT = "> ";

const char *savedata_filename = "savedata.tox";
const char *savedata_tmp_filename = "savedata.tox.tmp";

void create_tox()
{
    struct Tox_Options options;

    tox_options_default(&options);

    FILE *f = fopen(savedata_filename, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        char *savedata = malloc(fsize);

        fread(savedata, fsize, 1, f);
        fclose(f);

        options.savedata_type = TOX_SAVEDATA_TYPE_TOX_SAVE;
        options.savedata_data = (uint8_t*)savedata;
        options.savedata_length = fsize;

        tox = tox_new(&options, NULL);

        free(savedata);
    } else {
        tox = tox_new(&options, NULL);
    }
}

void update_savedata_file(const Tox *tox)
{
    size_t size = tox_get_savedata_size(tox);
    char *savedata = malloc(size);
    tox_get_savedata(tox, (uint8_t*)savedata);

    FILE *f = fopen(savedata_tmp_filename, "wb");
    fwrite(savedata, size, 1, f);
    fclose(f);

    rename(savedata_tmp_filename, savedata_filename);

    free(savedata);
}

void bootstrap(Tox *tox)
{
    DHT_node nodes[] =
    {
        {"178.62.250.138",             33445, "788236D34978D1D5BD822F0A5BEBD2C53C64CC31CD3149350EE27D4D9A2F9B6B", {0}},
        {"2a03:b0c0:2:d0::16:1",       33445, "788236D34978D1D5BD822F0A5BEBD2C53C64CC31CD3149350EE27D4D9A2F9B6B", {0}},
        {"tox.zodiaclabs.org",         33445, "A09162D68618E742FFBCA1C2C70385E6679604B2D80EA6E84AD0996A1AC8A074", {0}},
        {"163.172.136.118",            33445, "2C289F9F37C20D09DA83565588BF496FAB3764853FA38141817A72E3F18ACA0B", {0}},
        {"2001:bc8:4400:2100::1c:50f", 33445, "2C289F9F37C20D09DA83565588BF496FAB3764853FA38141817A72E3F18ACA0B", {0}},
        {"128.199.199.197",            33445, "B05C8869DBB4EDDD308F43C1A974A20A725A36EACCA123862FDE9945BF9D3E09", {0}},
        {"2400:6180:0:d0::17a:a001",   33445, "B05C8869DBB4EDDD308F43C1A974A20A725A36EACCA123862FDE9945BF9D3E09", {0}},
        {"node.tox.biribiri.org",      33445, "F404ABAA1C99A9D37D61AB54898F56793E1DEF8BD46B1038B9D822E8460FAB67", {0}}
    };

    for (size_t i = 0; i < sizeof(nodes)/sizeof(DHT_node); i ++) {
        sodium_hex2bin(nodes[i].key_bin, sizeof(nodes[i].key_bin),
                       nodes[i].key_hex, sizeof(nodes[i].key_hex)-1, NULL, NULL, NULL);
        tox_bootstrap(tox, nodes[i].ip, nodes[i].port, nodes[i].key_bin, NULL);
    }
}

void friend_request_cb(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length,
                                   void *user_data)
{
    tox_friend_add_norequest(tox, public_key, NULL);

    update_savedata_file(tox);
}

void receipt_callback(Tox *tox, uint32_t friend_number, uint32_t message_id, void *user_data) {
    printf("receipt received: %d, %d\n", friend_number, message_id);
}

void friend_message_cb(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                   size_t length, void *user_data)
{
    size_t name_sz = tox_friend_get_name_size(tox, friend_number, NULL);
    char *friend_name = calloc(1, name_sz+1);
    tox_friend_get_name(tox, friend_number, (uint8_t*)friend_name, NULL);

    if (IsTalking && friend_number == TalkingTo) {
        if (type == TOX_MESSAGE_TYPE_NORMAL) {
            uint8_t *tmp = calloc(1, length+1);
            memcpy(tmp, message, length);

            printf("\n%s: %s\n",friend_name, (char*)tmp);
            free(tmp);
        } else {
            printf("\n* receive MESSAGE ACTION type\n");
        }
    } else {
        printf("\n* receive message from %s\n",friend_name);
    }

    free(friend_name);
}

void self_connection_status_cb(Tox *tox, TOX_CONNECTION connection_status, void *user_data)
{
    switch (connection_status) {
        case TOX_CONNECTION_NONE:
            printf("Offline\n");
            break;
        case TOX_CONNECTION_TCP:
            printf("Online, using TCP\n");
            break;
        case TOX_CONNECTION_UDP:
            printf("Online, using UDP\n");
            break;
    }
    tox_connection_status = connection_status;
}

uint8_t *hex_string_to_bin(const char *hex_string)
{
    // byte is represented by exactly 2 hex digits, so lenth of binary string
    // is half of that of the hex one. only hex string with even length
    // valid. the more proper implementation would be to check if strlen(hex_string)
    // is odd and return error code if it is. we assume strlen is even. if it's not
    // then the last byte just won't be written in 'ret'.
    size_t i, len = strlen(hex_string) / 2;
    uint8_t *ret = (uint8_t *)malloc(len);
    const char *pos = hex_string;

    for (i = 0; i < len; ++i, pos += 2) {
        sscanf(pos, "%2hhx", &ret[i]);
    }

    return ret;
}

void setup_tox()
{
    create_tox();

    bootstrap(tox);

    tox_callback_friend_request(tox, friend_request_cb);
    tox_callback_friend_message(tox, friend_message_cb);

    tox_callback_self_connection_status(tox, self_connection_status_cb);

    update_savedata_file(tox);
}

//////// REPL


typedef void CommandHandler(int narg, char **args);

struct Command {
    char* name;
    char* desc;
    int   narg;
    CommandHandler *handler;
};

void command_help_helper(int narg, char **args);

void command_info_helper(int narg, char **args) {
    uint32_t addr_size = tox_address_size();
    uint8_t tox_id_bin[addr_size];
    tox_self_get_address(tox, tox_id_bin);

    char tox_id_hex[addr_size*2 + 1];
    sodium_bin2hex(tox_id_hex, sizeof(tox_id_hex), tox_id_bin, sizeof(tox_id_bin));

    for (size_t i = 0; i < sizeof(tox_id_hex)-1; i ++) {
        tox_id_hex[i] = toupper(tox_id_hex[i]);
    }

    size_t sz = tox_self_get_name_size(tox);
    char *name = calloc(1, sz);
    tox_self_get_name(tox, (uint8_t*)name);
    printf("Name:\t%s\n", name);
    free(name);

    printf("Tox ID:\t%s\n", tox_id_hex);

    sz = tox_self_get_status_message_size(tox);
    char *status = calloc(1, sz);
    tox_self_get_status_message(tox, (uint8_t*)status);
    printf("Status:\t%s\n",status);
    free(status);

    const char * conn_st = "offline";
    if (tox_connection_status == TOX_CONNECTION_UDP) {
        conn_st = "Online (UDP)";
    } else if (tox_connection_status == TOX_CONNECTION_TCP) {
        conn_st = "Online (TCP)";
    }
    printf("Network:\t%s\n",conn_st);
}

void command_setname_helper(int narg, char **args) {
    char *name = args[0];
    tox_self_set_name(tox, (uint8_t*)name, strlen(name), NULL);
}

void command_setstatus_helper(int narg, char **args) {
    char *status = args[0];
    tox_self_set_status_message(tox, (uint8_t*)status, strlen(status), NULL);
}

void command_add_helper(int narg, char **args) {
    char *hex_id = args[0];
    char *msg = "";
    if (narg > 1){
        msg = args[1];
    }

    uint8_t *bin_id = hex_string_to_bin(hex_id);
    tox_friend_add(tox, bin_id, (uint8_t*)msg, sizeof(msg), NULL);

    update_savedata_file(tox);

    free(bin_id);
}

void command_friends_helper(int narg, char **args) {
    size_t sz = tox_self_get_friend_list_size(tox);
    uint32_t *friend_list = malloc(sizeof(uint32_t) * sz);
    tox_self_get_friend_list(tox, friend_list);
    for (int i = 0;i<sz;i++) {
        uint32_t friend_num = friend_list[i];
        size_t name_sz = tox_friend_get_name_size(tox, friend_num, NULL);
        char *name = calloc(1, name_sz+1);
        int ret = tox_friend_get_name(tox, friend_num, (uint8_t*)name, NULL);

        size_t status_sz = tox_friend_get_status_message_size(tox, friend_num, NULL);
        char *status = calloc(1, status_sz+1);
        tox_friend_get_status_message(tox, friend_num, (uint8_t*)status, NULL);

        printf("%-3d%-20s%s\n",friend_num, name, status);

        free(name);
        free(status);
    }
    free(friend_list);
}

void command_save_helper(int narg, char **args) {
    update_savedata_file(tox);
}

void command_go_helper(int narg, char **args) {
    uint32_t friend_num = (uint32_t)atoi(args[0]);
    if (!tox_friend_exists(tox, friend_num)) {
        printf("friend not exist\n");
        return;
    }
    IsTalking = 1;
    TalkingTo = friend_num;
    REPL_PROMPT = "*> ";
}

#define COMMAND_ARGS_REST 100
struct Command commands[] = {
    {
        "help",
        "/help - print this message.",
        0,
        command_help_helper,
    },
    {
        "info",
        "/info - show your info",
        0,
        command_info_helper,
    },
    {
        "setname",
        "/setname <name> - set your name",
        1,
        command_setname_helper,
    },
    {
        "setstatus",
        "/setstatus <status_message> - set your status message.",
        1,
        command_setstatus_helper,
    },
    {
        "add",
        "/add <toxid> [<msg>] - add friend",
        1 + COMMAND_ARGS_REST,
        command_add_helper,
    },
    {
        "friends",
        "list your friends.",
        0,
        command_friends_helper,
    },
    {
        "save",
        "save your data.",
        0,
        command_save_helper,
    },
    {
        "go",
        "/go <friend_numer> - goto talk to someone",
        1,
        command_go_helper,
    }
};

void command_help_helper(int narg, char **args){
    for (int i=0;i<sizeof(commands)/sizeof(struct Command);i++) {
        printf("%-16s\t%s\n", commands[i].name, commands[i].desc);
    }
}

#define REPL_BUF_SIZE  1024

char replbuf[REPL_BUF_SIZE];
int nread = 0;

int readline(char *linebuf, int size){
    while (1) {
        ssize_t n = read(STDIN_FILENO, replbuf + nread, REPL_BUF_SIZE - nread);
        if (n<=0){
            return -1;
        }
        printf(">>>>read %zd\n", n);
        nread += n;
        for (int i=nread-n; i<nread; i++){
            if (replbuf[i] == '\n') {
                i++;
                memcpy(linebuf, replbuf, i);
                nread -= i;
                memmove(replbuf, replbuf+i, nread);
                memset(replbuf+nread, 0, REPL_BUF_SIZE-nread);
                return i;
            }
        }
    }
}

// caller makes sure args doesn't overflow
int parseline(char *line,char **tokens) {
    char dem = ' ';
    char *tok_begin = NULL;
    int n = 0;
    for (int i = 0;;i++) {
        if (line[i] == '\0'){
            if (tok_begin != NULL) {
                tokens[n++] = tok_begin;
            }
            return n;
        }

        if (line[i] != dem && tok_begin == NULL) {
            tok_begin = line + i;
        }

        if (line[i] == dem && tok_begin != NULL) {
            tokens[n++] = tok_begin;
            line[i] = '\0';
            tok_begin = NULL;
        }
    }
}

void repl_iterate(){
    static int firstcall = 1;
    if (firstcall) {
        firstcall = 0;
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        goto REPL_NEXT;
    }

    char linebuf[128];
    memset(linebuf,0,sizeof(linebuf));
    int ret = readline(linebuf, sizeof(linebuf));
    if (ret == -1) {
        return;
    }
    if (ret == 0){
        printf("\nbye.\n");
        exit(0);
    }
    char *buf = linebuf;
    buf[strlen(buf)-1] = '\0';
    if (buf[0] == '/') {
        buf++;
        char *tokens[COMMAND_ARGS_REST];
        int ntok = parseline(buf, tokens);
        char *cmdname = tokens[0];
        char **args = tokens + 1;
        int narg = ntok - 1;
        for (int i=0;i<sizeof(commands)/sizeof(struct Command);i++){
            struct Command *cmd = &commands[i];
            if (strcmp(cmd->name, cmdname) == 0) {
                if ((cmd->narg < COMMAND_ARGS_REST && cmd->narg != narg) || 
                        (cmd->narg >= COMMAND_ARGS_REST && (cmd->narg - COMMAND_ARGS_REST) < narg)){
                    printf("invalid args\n");
                } else {
                    cmd->handler(narg, args);
                }
                goto REPL_NEXT;
            }
        }
    } else if (IsTalking) {
        tox_friend_send_message(tox, TalkingTo, TOX_MESSAGE_TYPE_NORMAL, (uint8_t*)buf, strlen(buf), NULL);
        goto REPL_NEXT;
    }
    printf("Invalid command: %s, try `/help` instead\n", buf);
REPL_NEXT:
    fputs(REPL_PROMPT, stdout);
    fflush(stdout);
}

int main() {
    setbuf(stdin, NULL);
    printf("setup tox ...\n");
    setup_tox();

    while (1) {
        repl_iterate();
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox) * 1000);
    }
    tox_kill(tox);

    return 0;
}
