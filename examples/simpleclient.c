// gcc -o echo_bot echo_bot.c -std=gnu99 -lsodium libtoxcore.a
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include <sodium/utils.h>
#include <tox.h>

Tox *tox;

typedef struct DHT_node {
    const char *ip;
    uint16_t port;
    const char key_hex[TOX_PUBLIC_KEY_SIZE*2 + 1];
    unsigned char key_bin[TOX_PUBLIC_KEY_SIZE];
} DHT_node;

struct ConferenceUserData {
    uint32_t friend_number;
    const uint8_t *cookie;
    size_t length;
};

struct FriendUserData {
    uint8_t pubkey[TOX_PUBLIC_KEY_SIZE];
};

union RequestUserData {
    struct ConferenceUserData conference;
    struct FriendUserData friend;
};

struct Request {
    char *msg;
    uint32_t id;
    bool is_friend_request;
    union RequestUserData userdata;
    struct Request *next;
};
struct Request *requests = NULL;

struct ChatHistory {
    uint32_t friend_number;
    char *msg;
    struct ChatHistory *prev;
    struct ChatHistory *next;
};

struct ConferencePeer {
    char *name;
    char pubkey_hex[TOX_PUBLIC_KEY_SIZE * 2 +1];
    struct ConferencePeer *next;
};

struct Conference {
    uint32_t conference_number;
    char *title;
    size_t title_sz;
    uint32_t peer_count;
    struct ConferencePeer *peers;

    struct Conference *next;
};

struct Friend {
    uint32_t friend_number;
    char *name;
    int name_sz;
    char *status_message;
    int status_message_sz;
    TOX_CONNECTION connection;
    struct ChatHistory *hist;

    struct Friend *next;
};

struct Friend *friends = NULL;

struct Friend self;

struct Conference *conferences = NULL;

// I assume normal friend_number will not get to this value, for code's simplicity. Plz do not do this in any serious client.
#define SELF_FRIENDNUM UINT32_MAX

uint32_t TalkingTo = SELF_FRIENDNUM;

const char *savedata_filename = "savedata.tox";
const char *savedata_tmp_filename = "savedata.tox.tmp";

#define LINE_MAX_SIZE 1024

#define CODE_ERASE_LINE    "\r\033[2K" 

#define RESET_COLOR        "\x01b[0m"
#define SELF_TALK_COLOR    "\x01b[35m"  // magenta
#define GUEST_TALK_COLOR   "\x01b[90m" // bright black
#define CMD_PROMPT_COLOR   "\x01b[34m" // blue

#define CMD_PROMPT   CMD_PROMPT_COLOR "> " RESET_COLOR // green
#define TALK_PROMPT  CMD_PROMPT_COLOR ">> %-.12s : " RESET_COLOR

#define GUEST_MSG_PREFIX  GUEST_TALK_COLOR "%s  %12.12s | " RESET_COLOR
#define SELF_MSG_PREFIX  SELF_TALK_COLOR "%s  %12.12s | " RESET_COLOR
#define CMD_MSG_PREFIX  CMD_PROMPT

#define PRINT(_fmt, ...) \
    fputs(CODE_ERASE_LINE,stdout);\
    printf(_fmt "\n", ##__VA_ARGS__);

#define COLOR_PRINT(_color, _fmt,...) PRINT(_color _fmt RESET_COLOR, ##__VA_ARGS__)

#define INFO(_fmt,...) COLOR_PRINT("\x01b[36m", _fmt, ##__VA_ARGS__)  // cyran
#define WARN(_fmt,...) COLOR_PRINT("\x01b[33m", _fmt, ##__VA_ARGS__) // yellow
#define ERROR(_fmt,...) COLOR_PRINT("\x01b[31m", _fmt, ##__VA_ARGS__) // red


///////////////////////////////
// Utils
////////////////////////////////

#define RESIZE(key, size_key, length) \
    if ((size_key) < (length + 1)) { \
        size_key = (length+1);\
        key = calloc(1, size_key);\
    }

#define LIST_FIND(_p, _condition) \
    for (;*(_p) != NULL;_p = &((*_p)->next)) { \
        if (_condition) { \
            break;\
        }\
    }\

char* getftime() {
    static char timebuf[64];

    time_t tt = time(NULL);
    struct tm *tm = localtime(&tt);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);
    return timebuf;
}

const char * connection_enum2text(TOX_CONNECTION conn) {
    switch (conn) {
        case TOX_CONNECTION_NONE:
            return "Offline";
        case TOX_CONNECTION_TCP:
            return "Online(TCP)";
        case TOX_CONNECTION_UDP:
            return "Online(UDP)";
        default:
            return "UNKNOWN";
    }
}

struct Friend *getfriend(uint32_t friend_number) {
    struct Friend **p = &friends;
    LIST_FIND(p, (*p)->friend_number == friend_number);
    return *p;
}

struct Friend *addfriend(uint32_t friend_number) {
    struct Friend *f = calloc(1, sizeof(struct Friend));
    f->next = friends;
    friends = f;
    f->friend_number = friend_number;
    f->connection = TOX_CONNECTION_NONE;
    return f;
}


bool delfriend(uint32_t friend_number) {
    struct Friend **p = &friends;
    LIST_FIND(p, (*p)->friend_number == friend_number);
    struct Friend *f = *p;
    if (f) {
        *p = f->next;
        if (f->name) free(f->name);
        if (f->status_message) free(f->status_message);
        free(f);
        return 1;
    }
    return 0;
}

struct Conference *addconfer(uint32_t conference_number) {
    struct Conference *cf = calloc(1, sizeof(struct Conference));
    cf->next = conferences;
    conferences = cf;

    cf->conference_number = conference_number;

    return cf;
}

struct Conference *getconfer(uint32_t conference_number) {
    struct Conference **p = &conferences;
    LIST_FIND(p, (*p)->conference_number == conference_number);
    return *p;
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

char *bin2hex(const uint8_t *bin, size_t length) {
    size_t hexlen = 2*length + 1;
    char *hex = malloc(hexlen);
    sodium_bin2hex(hex, hexlen, bin, length);

    for (size_t i = 0; i < hexlen-1; i++) {
        hex[i] = toupper(hex[i]);
    }

    return hex;
}

//////////////////////////
// Async REPL
/// //////////////////////

struct AsyncREPL {
    char *line;
    char *prompt;
    size_t sz;
    int  nbuf;
    int nstack;
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
    if (arepl->nbuf > 0) printf("%.*s", arepl->nbuf, arepl->line);
    if (arepl->nstack > 0) {
        printf("%.*s",(int)arepl->nstack, arepl->line + arepl->sz - arepl->nstack);
        printf("\033[%dD",arepl->nstack); // move cursor
    }
    fflush(stdout);
}

#define _AREPL_CURSOR_LEFT() arepl->line[arepl->sz - (++arepl->nstack)] = arepl->line[--arepl->nbuf]
#define _AREPL_CURSOR_RIGHT() arepl->line[arepl->nbuf++] = arepl->line[arepl->sz - (arepl->nstack--)]

int arepl_readline(struct AsyncREPL *arepl, char c, char *line, size_t sz){
    static uint32_t escaped = 0;
    if (c == '\033') { // mark escape code
        escaped = 1;
        return 0;
    }

    if (escaped>0) escaped++;

    switch (c) {
        case '\n': {
            int ret = snprintf(line, sz, "%.*s%.*s\n",(int)arepl->nbuf, arepl->line, (int)arepl->nstack, arepl->line + arepl->sz - arepl->nstack);
            arepl->nbuf = 0;
            arepl->nstack = 0;
            return ret;
        }

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

        case 'D':
        case 'C':
            if (escaped == 3 && arepl->nbuf >= 1 && arepl->line[arepl->nbuf-1] == '[') { // arrow keys
                arepl->nbuf--;
                if (c == 'D' && arepl->nbuf > 0) _AREPL_CURSOR_LEFT(); // left arrow
                if (c == 'C' && arepl->nstack > 0) _AREPL_CURSOR_RIGHT(); // right arrow
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
    INFO("* receive friend request(use `/accept` to see).");

    struct Request *req = malloc(sizeof(struct Request));

    req->id = 1 + ((requests != NULL) ? requests->id : 0);
    memcpy(req->userdata.friend.pubkey, public_key, TOX_PUBLIC_KEY_SIZE);
    sprintf(req->msg, "%.*s", (int)length, (char*)message);

    req->next = requests;
    requests = req;
}

void receipt_callback(Tox *tox, uint32_t friend_number, uint32_t message_id, void *user_data) {
    printf("receipt received: %d, %d\n", friend_number, message_id);
}

void friend_message_cb(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                   size_t length, void *user_data)
{
    struct Friend *f = getfriend(friend_number);
    if (!f) return;

    if (friend_number == TalkingTo) {
        if (type == TOX_MESSAGE_TYPE_NORMAL) {
            PRINT(GUEST_MSG_PREFIX "%.*s", getftime(), f->name, (int)length, (char*)message);
        } else {
            INFO("* receive MESSAGE ACTION type");
        }
    } else {
        INFO("* receive message from %s\n",f->name);
    }
}


void friend_name_cb(Tox *tox, uint32_t friend_number, const uint8_t *name, size_t length, void *user_data) {
    struct Friend *f = getfriend(friend_number);

    if (f) {
        RESIZE(f->name, f->name_sz, length);
        memcpy(f->name, name, length);
    }
}

void friend_status_message_cb(Tox *tox, uint32_t friend_number, const uint8_t *message, size_t length, void *user_data) {
    struct Friend *f = getfriend(friend_number);
    if (f) {
        RESIZE(f->status_message, f->status_message_sz, length);
        memcpy(f->status_message, message, length);
    }
}


void friend_connection_status_cb(Tox *tox, uint32_t friend_number, TOX_CONNECTION connection_status, void *user_data)
{
    struct Friend *f = getfriend(friend_number);
    if (f) {
        f->connection = connection_status;
        INFO("* %s is %s", f->name, connection_enum2text(connection_status));
    }
}

void self_connection_status_cb(Tox *tox, TOX_CONNECTION connection_status, void *user_data)
{
    self.connection = connection_status;
    INFO("* You are %s", connection_enum2text(connection_status));
}

void conference_invite_cb(Tox *tox, uint32_t friend_number, TOX_CONFERENCE_TYPE type, const uint8_t *cookie, size_t length, void *user_data) {
    struct Friend *f = getfriend(friend_number);
    if (f) {
        if (type == TOX_CONFERENCE_TYPE_AV) {
            WARN("* %s invites you to an AV conference, which has not been supported.", f->name);
            return;
        }
        INFO("* %s invites you to a conference(try `/accept` to see)",f->name);
        struct Request *req = malloc(sizeof(struct Request));
        req->id = 1 + ((requests != NULL) ? requests->id : 0);
        req->next = requests;
        requests = req;

        req->is_friend_request = false;
        req->userdata.conference.cookie = cookie;
        req->userdata.conference.length = length;
        req->userdata.conference.friend_number = friend_number;
        req->msg = malloc(f->name_sz);
        memcpy(req->msg, f->name, f->name_sz);
    }
}

void conference_title_cb(Tox *tox, uint32_t conference_number, uint32_t peer_number, const uint8_t *title, size_t length, void *user_data) {
    struct Conference *cf = getconfer(conference_number);
    if (cf) {
        RESIZE(cf->title, cf->title_sz, length);
        memcpy(cf->title, title, length);
    }
}

void conference_message_cb(Tox *tox, uint32_t conference_number, uint32_t peer_number, TOX_MESSAGE_TYPE type, const uint8_t *message, size_t length, void *user_data) {
    struct Conference *cf = getconfer(conference_number);
    if (cf) {
    }
}

void create_tox()
{
    struct Tox_Options *options = tox_options_new(NULL);

    FILE *f = fopen(savedata_filename, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        char *savedata = malloc(fsize);

        fread(savedata, fsize, 1, f);
        fclose(f);

        tox_options_set_savedata_type(options, TOX_SAVEDATA_TYPE_TOX_SAVE);
        tox_options_set_savedata_data(options, (uint8_t*)savedata, fsize);

        tox = tox_new(options, NULL);

        free(savedata);
    } else {
        tox = tox_new(options, NULL);
    }
}

void init_friends() {
    size_t sz = tox_self_get_friend_list_size(tox);
    uint32_t *friend_list = malloc(sizeof(uint32_t) * sz);
    tox_self_get_friend_list(tox, friend_list);

    for (int i = 0;i<sz;i++) {
        uint32_t friend_num = friend_list[i];
        struct Friend *f = addfriend(friend_num);

        f->name_sz = tox_friend_get_name_size(tox, friend_num, NULL) + 1;
        f->name = calloc(1, f->name_sz);
        tox_friend_get_name(tox, friend_num, (uint8_t*)f->name, NULL);

        f->status_message_sz = tox_friend_get_status_message_size(tox, friend_num, NULL) + 1;
        f->status_message = calloc(1, f->status_message_sz);
        tox_friend_get_status_message(tox, friend_num, (uint8_t*)f->status_message, NULL);

    }
    free(friend_list);

    // add self
    self.friend_number = SELF_FRIENDNUM;
    self.name_sz = tox_self_get_name_size(tox) + 1;
    self.name = calloc(1, self.name_sz);
    tox_self_get_name(tox, (uint8_t*)self.name);

    self.status_message_sz = tox_self_get_status_message_size(tox) + 1;
    self.status_message = calloc(1, self.status_message_sz);
    tox_self_get_status_message(tox, (uint8_t*)self.status_message);
}

void setup_tox()
{
    create_tox();

    init_friends();

    bootstrap(tox);

    // self
    tox_callback_self_connection_status(tox, self_connection_status_cb);

    // friend
    tox_callback_friend_request(tox, friend_request_cb);
    tox_callback_friend_message(tox, friend_message_cb);
    tox_callback_friend_name(tox, friend_name_cb);
    tox_callback_friend_status_message(tox, friend_status_message_cb); 
    tox_callback_friend_connection_status(tox, friend_connection_status_cb);

    // conference
    tox_callback_conference_invite(tox, conference_invite_cb);
    tox_callback_conference_title(tox, conference_title_cb);
    tox_callback_conference_message(tox, conference_message_cb);


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
    PRINT("Name:\t%s", self.name);

    uint32_t addr_size = tox_address_size();
    uint8_t tox_id_bin[addr_size];
    tox_self_get_address(tox, tox_id_bin);
    char *hex = bin2hex(tox_id_bin, sizeof(tox_id_bin));
    PRINT("Tox ID:\t%s", hex);
    free(hex);

    PRINT("Status Message:\t%s",self.status_message);
    PRINT("Network:\t%s",connection_enum2text(self.connection));
}

void command_setname_helper(int narg, char **args) {
    char *name = args[0];
    size_t len = strlen(name);
    tox_self_set_name(tox, (uint8_t*)name, strlen(name), NULL);

    RESIZE(self.name, self.name_sz, len);
    memcpy(self.name, name, len);
}

void command_setstatus_helper(int narg, char **args) {
    char *status = args[0];
    size_t len = strlen(status);
    tox_self_set_status_message(tox, (uint8_t*)status, strlen(status), NULL);

    RESIZE(self.status_message, self.status_message_sz, len);
    memcpy(self.status_message, status, len);
}

void command_add_helper(int narg, char **args) {
    char *hex_id = args[0];
    char *msg = "";
    if (narg > 1){
        msg = args[1];
    }

    uint8_t *bin_id = hex_string_to_bin(hex_id);
    uint32_t friend_number = tox_friend_add(tox, bin_id, (uint8_t*)msg, sizeof(msg), NULL);
    free(bin_id);

    addfriend(friend_number);
}

void command_del_helper(int narg, char **args) {
    uint32_t friend_number = (uint32_t)atoi(args[0]);
    if (delfriend(friend_number)) {
        tox_friend_delete(tox, friend_number, NULL);
    } else {
        ERROR("! friend not exist");
    }
}

void command_contacts_helper(int narg, char **args) {
    struct Friend *f = friends;
    PRINT("Friends:");
    for (;f != NULL; f = f->next) {
        PRINT("%4d %15.15s  %12.12s  %s",f->friend_number, f->name, connection_enum2text(f->connection), f->status_message);
    }

    PRINT("Conferences:");
    struct Conference *cf = conferences;
    for (;cf != NULL; cf = cf->next) {
        PRINT("%4d%6d(peers)  %s",cf->conference_number, cf->peer_count, cf->title);
    }
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
    struct Friend *f = getfriend(friend_num);
    if (!f) {
        ERROR("! friend not exist");
        return;
    }
    TalkingTo = friend_num;
    sprintf(async_repl->prompt, TALK_PROMPT, f->name);
    INFO("* talk to %s", f->name);
}

void _command_accept(int narg, char **args, bool is_accept) {
    if (narg == 0) {
        struct Request * req = requests;
        for (;req != NULL;req=req->next) {
            PRINT("%-9u%-12s%s", req->id, (req->is_friend_request ? "FRIEND" : "CONFERENCE"), req->msg);
        }
    } else {
        uint32_t id = (uint32_t)atoi(args[0]);
        struct Request **p = &requests;
        LIST_FIND(p, (*p)->id == id);
        struct Request *req = *p;
        if (req) {
            *p = req->next;
            if (is_accept) {
                if (req->is_friend_request) {
                    uint32_t friend_num = tox_friend_add_norequest(tox, req->userdata.friend.pubkey, NULL);
                    if (friend_num == UINT32_MAX) {
                        ERROR("! accept friend request failed");
                    } else {
                        addfriend(friend_num);
                    }
                } else { // conference invite
                    struct ConferenceUserData *data = &req->userdata.conference;
                    uint32_t conference_number = tox_conference_join(tox, data->friend_number, data->cookie, data->length, NULL);
                    if (conference_number == UINT32_MAX) {
                        ERROR("! join conference failed");
                    } else {
                        addconfer(conference_number);
                    }
                }
            }
            free(req->msg);
            free(req);
        } else {
            ERROR("! Invalid id");
        }
    }
}

void command_accept_helper(int narg, char **args) {
    _command_accept(narg, args, 1);
}

void command_deny_helper(int narg, char **args) {
    _command_accept(narg, args, 0);
}

#define COMMAND_ARGS_REST 100
#define COMMAND_LENGTH (sizeof(commands)/sizeof(struct Command))

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
        "del",
        "<friend_number> - del a friend.",
        1,
        command_del_helper,
    },
    {
        "contacts",
        "- list your friends.",
        0,
        command_contacts_helper,
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
    },
    {
        "accept",
        "[<request_number>] - list friend requests or conference invites.",
        0 + COMMAND_ARGS_REST,
        command_accept_helper,
    },
    {
        "deny",
        "[<request_number>] - list friend requests or conference invites.",
        0 + COMMAND_ARGS_REST,
        command_accept_helper,
    },
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
            if (!arepl_readline(async_repl, c, line, sizeof(line))) continue;

            int len = strlen(line);
            line[--len] = '\0'; // remove trailing \n

            if (line[0] == '/') {
                char *tokens[COMMAND_ARGS_REST];
                int ntok = parseline(line+1, tokens); // skip leading '/'
                int j = 0;
                for (;j<COMMAND_LENGTH;j++){
                    if (strcmp(commands[j].name, tokens[0]) == 0) {
                        PRINT(CMD_MSG_PREFIX "%.*s", len, line);
                        commands[j].handler(ntok-1, tokens+1);
                        break;
                    }
                }
                if (j != COMMAND_LENGTH) continue;
            }

            if (TalkingTo != SELF_FRIENDNUM) {  // in talk mode
                PRINT(SELF_MSG_PREFIX "%.*s", getftime(), self.name, len, line);
                tox_friend_send_message(tox, TalkingTo, TOX_MESSAGE_TYPE_NORMAL, (uint8_t*)line, strlen(line), NULL);
            } else {
                WARN("Invalid command: %s, try `/help` instead.", line);
            }
        } // end for
    } // end while
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
