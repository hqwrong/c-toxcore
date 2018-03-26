// gcc -o echo_bot echo_bot.c -std=gnu99 -lsodium libtoxcore.a
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <termios.h>
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

// I assume normal friend_number will not get to this value, for code's simplicity. Plz do not do this in any serious client.
#define SELF_FRIENDNUM ~((uint32_t)0) 

uint32_t TalkingTo = SELF_FRIENDNUM;

const char *savedata_filename = "savedata.tox";
const char *savedata_tmp_filename = "savedata.tox.tmp";

#define LINE_MAX_SIZE 1024

#define CODE_ERASE_LINE    "\r\033[2K" 

#define RESET_COLOR        "\x01b[0m"
#define SELF_TALK_COLOR    "\x01b[32m"  // green
#define GUEST_TALK_COLOR   "\x01b[35m" // magenta
#define CMD_PROMPT_COLOR   "\x01b[32m" // green

#define CMD_PROMPT   CMD_PROMPT_COLOR "> " RESET_COLOR // green
#define GUEST_TALK_PROMPT  GUEST_TALK_COLOR "%-.12s | " RESET_COLOR
#define SELF_TALK_PROMPT   SELF_TALK_COLOR "%-.12s | " RESET_COLOR

#define TALK_PROMPT_TIME_FORMAT  "hh:mm:ss"  // comment this line to disable display time

#define PRINT(_fmt, ...) \
    fputs(CODE_ERASE_LINE,stdout);\
    printf(_fmt "\n", ##__VA_ARGS__);

#define COLOR_PRINT(_color, _fmt,...) PRINT(_color _fmt RESET_COLOR, ##__VA_ARGS__)

#define INFO(_fmt,...) COLOR_PRINT("\x01b[36m", _fmt, ##__VA_ARGS__)  // cyran
#define WARN(_fmt,...) COLOR_PRINT("\x01b[33m", _fmt, ##__VA_ARGS__) // yellow
#define ERROR(_fmt,...) COLOR_PRINT("\x01b[31m", _fmt, ##__VA_ARGS__) // red


//////////////////////////
// Async REPL
/// //////////////////////

struct AsyncREPL {
    char *line;
    char *prompt;
    size_t sz;
    size_t nbuf;
    size_t nstack;
};

struct termios saved_tattr;

struct AsyncREPL *async_repl;

void setup_arepl() {
    async_repl = malloc(sizeof(struct AsyncREPL));
    async_repl->nbuf = 0;
    async_repl->nstack = 0;
    async_repl->sz = LINE_MAX_SIZE;
    async_repl->line = malloc(LINE_MAX_SIZE);
    async_repl->prompt = malloc(LINE_MAX_SIZE);

    strcpy(async_repl->prompt, CMD_PROMPT);

    /* Set the Non-Canonical terminal mode. */
    struct termios tattr;
    tcgetattr(STDIN_FILENO, &tattr);
    saved_tattr = tattr;  // save it to restore when exit
    tattr.c_lflag &= ~(ICANON|ECHO); /* Clear ICANON. */
    tattr.c_cc[VMIN] = 1;
    tattr.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &tattr);

    /* Set Non-Blocking stdin */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

void arepl_reprint(struct AsyncREPL *arepl) {
    fputs(CODE_ERASE_LINE, stdout);
    if (arepl->prompt) fputs(arepl->prompt, stdout);
    if (arepl->nbuf > 0) printf("%.*s", (int)arepl->nbuf, arepl->line);
    if (arepl->nstack > 0) {
        printf("%.*s",(int)arepl->nstack, arepl->line - arepl->nstack);
        printf("\033[%zuD",arepl->nstack); // move cursor
    }
    fflush(stdout);
}

#define _AREPL_CURSOR_LEFT() arepl->line[arepl->sz - (arepl->nstack++)] = arepl->line[--arepl->nbuf]
#define _AREPL_CURSOR_RIGHT() arepl->line[arepl->nbuf++] = arepl->line[arepl->sz - (--arepl->nstack)]

int arepl_readline(struct AsyncREPL *arepl, char c, char *line, size_t sz){
    switch (c) {
        case '\n':
            putchar('\n'); // open a new line
            int ret = snprintf(line, sz, "%.*s%.*s\n",(int)arepl->nbuf, arepl->line, (int)arepl->nstack, arepl->line - arepl->nstack);
            arepl->nbuf = 0;
            arepl->nstack = 0;
            return ret;

        case '\010':  // C-h
        case '\177':  // Backspace
            if (arepl->nbuf > 0) arepl->nbuf--;
            break;
        case '\025': // C-u
            arepl->nbuf = 0;
            break;
        case '\013': // C-k Vertical Tab
            arepl->nstack = 0;
            break;
        case '\001': // C-a
            while (arepl->nbuf > 0) _AREPL_CURSOR_LEFT();
            break;
        case '\005': // C-e
            while (arepl->nstack > 0) _AREPL_CURSOR_RIGHT();
            break;
        case '\002': // C-b
            if (arepl->nbuf > 0) _AREPL_CURSOR_LEFT();
            break;
        case '\006': // C-f
            if (arepl->nstack > 0) _AREPL_CURSOR_RIGHT();
            break;
        case '\027': // C-w: backward delete a word
            while (arepl->nbuf>0 && arepl->line[arepl->nbuf-1] == ' ') arepl->nbuf--;
            while (arepl->nbuf>0 && arepl->line[arepl->nbuf-1] != ' ') arepl->nbuf--;
            break;
        case '\104':
        case '\103':
            if (arepl->nbuf >= 2 && strncmp(arepl->line + arepl->nbuf - 2,"\033\133",2) == 0) { // left or right arrow
                arepl->nbuf -= 2;
                if (c == '\104' && arepl->nbuf > 0) _AREPL_CURSOR_LEFT(); // left arrow
                if (c == '\103' && arepl->nstack > 0) _AREPL_CURSOR_RIGHT(); // right arrow
                break;
            }
            // fall through to default case
        default:
            arepl->line[arepl->nbuf++] = c;
    }
    return 0;
}

////////////////////////
// tox
////////////////////////

char* get_name(uint32_t friend_number){
    static char *namebuf = NULL;
    static uint32_t namebuf_size = 0;

    size_t len = 1;
    if (friend_number == SELF_FRIENDNUM) { // self
        len += tox_self_get_name_size(tox);
    } else {
        TOX_ERR_FRIEND_QUERY err;
        size_t namesz = tox_friend_get_name_size(tox, friend_number, &err);
        if (err != TOX_ERR_FRIEND_QUERY_OK) {
            ERROR("! `tox_friend_get_name_size` return err, errcode:%d", err);
            namebuf[0] = '\0';
            return namebuf;
        }
        len += namesz;
    }
    if (len > namebuf_size) {
        namebuf = realloc(namebuf, len);
        namebuf_size = len;
    }
    if (friend_number == SELF_FRIENDNUM) {
        tox_self_get_name(tox, (uint8_t*)namebuf);
    } else {
        TOX_ERR_FRIEND_QUERY err;
        if (!tox_friend_get_name(tox, friend_number, (uint8_t*)namebuf, &err)) {
            ERROR("! `tox_friend_get_name` failed, errcode: %d", err)
        }
    } 
    namebuf[len-1] = '\0';

    return namebuf;
}

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
    char *friend_name = get_name(friend_number);

    if (friend_number == TalkingTo) {
        if (type == TOX_MESSAGE_TYPE_NORMAL) {
            PRINT(GUEST_TALK_PROMPT "%.*s", friend_name, (int)length, (char*)message);
        } else {
            INFO("* receive MESSAGE ACTION type");
        }
    } else {
        INFO("* receive message from %s\n",friend_name);
    }
}

void friend_connection_status_cb(Tox *tox, uint32_t friend_number, TOX_CONNECTION connection_status, void *user_data)
{
    char *name = get_name(friend_number);

    switch (connection_status) {
        case TOX_CONNECTION_NONE:
            INFO("* %s is Offline", name);
            break;
        case TOX_CONNECTION_TCP:
            INFO("* %s is Online(TCP)", name);
            break;
        case TOX_CONNECTION_UDP:
            INFO("* %s is Online(UDP)", name);
            break;
    }
}

void self_connection_status_cb(Tox *tox, TOX_CONNECTION connection_status, void *user_data)
{
    switch (connection_status) {
        case TOX_CONNECTION_NONE:
            WARN("* you are Offline");
            break;
        case TOX_CONNECTION_TCP:
            INFO("* you are Online(TCP)");
            break;
        case TOX_CONNECTION_UDP:
            INFO("* you are Online(UDP)");
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
    tox_callback_friend_connection_status(tox, friend_connection_status_cb);

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

    char *name = get_name(SELF_FRIENDNUM);
    PRINT("Name:\t%s", name);
    PRINT("Tox ID:\t%s", tox_id_hex);

    size_t sz = tox_self_get_status_message_size(tox);
    char *status = calloc(1, sz);
    tox_self_get_status_message(tox, (uint8_t*)status);
    PRINT("Status:\t%s",status);
    free(status);

    const char * conn_st = "Offline";
    if (tox_connection_status == TOX_CONNECTION_UDP) {
        conn_st = "Online (UDP)";
    } else if (tox_connection_status == TOX_CONNECTION_TCP) {
        conn_st = "Online (TCP)";
    }
    PRINT("Network:\t%s",conn_st);
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
        char *name = get_name(friend_num);

        size_t status_sz = tox_friend_get_status_message_size(tox, friend_num, NULL);
        char *status = calloc(1, status_sz+1);
        tox_friend_get_status_message(tox, friend_num, (uint8_t*)status, NULL);

        PRINT("%-3d%-20s%s",friend_num, name, status);

        free(name);
        free(status);
    }
    free(friend_list);
}

void command_save_helper(int narg, char **args) {
    update_savedata_file(tox);
}

void command_go_helper(int narg, char **args) {
    if (narg == 0) {
        TalkingTo = SELF_FRIENDNUM;
        strcpy(async_repl->prompt, CMD_PROMPT);
        return;
    }
    uint32_t friend_num = (uint32_t)atoi(args[0]);
    if (!tox_friend_exists(tox, friend_num)) {
        ERROR("! friend not exist");
        return;
    }
    TalkingTo = friend_num;
    sprintf(async_repl->prompt, SELF_TALK_PROMPT, get_name(SELF_FRIENDNUM));
    INFO("* talk to %s", get_name(friend_num));
}

#define COMMAND_ARGS_REST 100
struct Command commands[] = {
    {
        "help",
        "- print this message.",
        0,
        command_help_helper,
    },
    {
        "info",
        "- show your info",
        0,
        command_info_helper,
    },
    {
        "setname",
        "<name> - set your name",
        1,
        command_setname_helper,
    },
    {
        "setstatus",
        "<status_message> - set your status message.",
        1,
        command_setstatus_helper,
    },
    {
        "add",
        "<toxid> [<msg>] - add friend",
        1 + COMMAND_ARGS_REST,
        command_add_helper,
    },
    {
        "friends",
        "- list your friends.",
        0,
        command_friends_helper,
    },
    {
        "save",
        "- save your data.",
        0,
        command_save_helper,
    },
    {
        "go",
        "[<friend_number>] - goto talk to someone if spcified <friend_number> or goto cmd mode.",
        1,
        command_go_helper,
    }
};

void command_help_helper(int narg, char **args){
    for (int i=0;i<sizeof(commands)/sizeof(struct Command);i++) {
        PRINT("%-16s\t%s", commands[i].name, commands[i].desc);
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
    static char buf[128];
    static char line[LINE_MAX_SIZE];
    while (1) {
        int n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        for (int i=0;i<n;i++) {
            char c = buf[i];
            if (c == '\004')          /* C-d */
                exit(0);
            if (arepl_readline(async_repl, c, line, sizeof(line))) {
                char *l = line;
                l[strlen(l)-1] = '\0'; // remove trailing \n
                if (l[0] == '/') {
                    l++;
                    char *tokens[COMMAND_ARGS_REST];
                    int ntok = parseline(l, tokens);
                    struct Command *cmd = NULL;
                    for (int i=0;i<sizeof(commands)/sizeof(struct Command);i++){
                        if (strcmp(commands[i].name, tokens[0]) == 0) {
                            cmd = &commands[i];
                            break;
                        }
                    }
                    if (cmd) {
                        cmd->handler(ntok-1, tokens+1);
                    } else {
                        WARN("Invalid command: %s, try `/help` instead.", l);
                    }
                } else if (TalkingTo != SELF_FRIENDNUM) {
                    tox_friend_send_message(tox, TalkingTo, TOX_MESSAGE_TYPE_NORMAL, (uint8_t*)l, strlen(l), NULL);
                }
            }
        }
    }
    arepl_reprint(async_repl);
}

void exit_cb() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tattr);
    update_savedata_file(tox);
}

int main() {
    INFO("setup tox ...");

    setup_arepl();
    setup_tox();

    atexit(exit_cb);

    uint32_t msecs = 0;
    while (1) {
        if (msecs > 20) { // every 0.02 secs
            msecs = 0;
            repl_iterate();
        }
        tox_iterate(tox, NULL);
        uint32_t v = tox_iteration_interval(tox);
        msecs += v;
        usleep(v * 1000);
    }

    return 0;
}
