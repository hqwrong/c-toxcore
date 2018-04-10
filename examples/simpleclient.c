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

#include <tox.h>

Tox *tox;

typedef struct DHT_node {
    const char *ip;
    uint16_t port;
    const char key_hex[TOX_PUBLIC_KEY_SIZE*2 + 1];
    uint8_t *key_bin;
} DHT_node;

struct ConferenceUserData {
    uint32_t friend_number;
    uint8_t *cookie;
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

struct ConferPeer {
    uint8_t pubkey[TOX_PUBLIC_KEY_SIZE];
    char name[TOX_MAX_NAME_LENGTH + 1];
};

struct Conference {
    uint32_t conference_number;
    char *title;
    size_t title_sz;
    struct ConferPeer *peers;
    size_t peers_count;

    struct Conference *next;
};

struct Friend {
    uint32_t friend_number;
    char *name;
    int name_sz;
    char *status_message;
    int status_message_sz;
    TOX_CONNECTION connection;

    struct Friend *next;
};

struct Friend *friends = NULL;

struct Friend self;

struct Conference *conferences = NULL;

// I assume normal friend_number will not get to this value, for code's simplicity. Plz do not do this in any serious client.
enum TALK_TYPE { TALK_TYPE_FRIEND, TALK_TYPE_CONFERENCE, TALK_TYPE_COUNT, TALK_TYPE_NULL = UINT32_MAX };

uint32_t TalkingTo = TALK_TYPE_NULL;

const char *savedata_filename = "savedata.tox";
const char *savedata_tmp_filename = "savedata.tox.tmp";

#define LINE_MAX_SIZE 1024

#define PORT_RANGE_START 33445
#define PORT_RANGE_END   34445

#define CODE_ERASE_LINE    "\r\033[2K" 

#define RESET_COLOR        "\x01b[0m"
#define SELF_TALK_COLOR    "\x01b[35m"  // magenta
#define GUEST_TALK_COLOR   "\x01b[90m" // bright black
#define CMD_PROMPT_COLOR   "\x01b[34m" // blue

#define CMD_PROMPT   CMD_PROMPT_COLOR "> " RESET_COLOR // green
#define FRIEND_TALK_PROMPT  CMD_PROMPT_COLOR "%-.12s << " RESET_COLOR
#define CONFERENCE_TALK_PROMPT  CMD_PROMPT_COLOR "%-.12s <<< " RESET_COLOR

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

char *get_confer_peername(uint32_t conference_number, uint32_t peer_number) {
    static char *peername = NULL;
    if (!peername) peername = malloc(tox_max_name_length() + 1);
    TOX_ERR_CONFERENCE_PEER_QUERY err;
    size_t sz = tox_conference_peer_get_name_size(tox, conference_number, peer_number, &err);
    if (err != TOX_ERR_CONFERENCE_PEER_QUERY_OK) return NULL;
    tox_conference_peer_get_name(tox, conference_number, peer_number, (uint8_t*)peername, NULL);
    peername[sz] = '\0';
    return peername;
}

uint8_t *hex2bin(const char *hex)
{
    size_t len = strlen(hex) / 2;
    uint8_t *bin = malloc(len);

    for (size_t i = 0; i < len; ++i, hex += 2) {
        sscanf(hex, "%2hhx", &bin[i]);
    }

    return bin;
}

char *bin2hex(const uint8_t *bin, size_t length) {
    char *hex = malloc(2*length + 1);
    char *saved = hex;
    for (int i=0; i<length;i++,hex+=2) {
        sprintf(hex, "%02X",bin[i]);
    }
    return saved;
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
                if (c == 'D' && arepl->nbuf > 0) _AREPL_CURSOR_LEFT(); // left arrow: \033[D
                if (c == 'C' && arepl->nstack > 0) _AREPL_CURSOR_RIGHT(); // right arrow: \033[C
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
        {"178.62.250.138",             33445, "788236D34978D1D5BD822F0A5BEBD2C53C64CC31CD3149350EE27D4D9A2F9B6B", NULL},
        {"2a03:b0c0:2:d0::16:1",       33445, "788236D34978D1D5BD822F0A5BEBD2C53C64CC31CD3149350EE27D4D9A2F9B6B", NULL},
        {"tox.zodiaclabs.org",         33445, "A09162D68618E742FFBCA1C2C70385E6679604B2D80EA6E84AD0996A1AC8A074", NULL},
        {"163.172.136.118",            33445, "2C289F9F37C20D09DA83565588BF496FAB3764853FA38141817A72E3F18ACA0B", NULL},
        {"2001:bc8:4400:2100::1c:50f", 33445, "2C289F9F37C20D09DA83565588BF496FAB3764853FA38141817A72E3F18ACA0B", NULL},
        {"128.199.199.197",            33445, "B05C8869DBB4EDDD308F43C1A974A20A725A36EACCA123862FDE9945BF9D3E09", NULL},
        {"2400:6180:0:d0::17a:a001",   33445, "B05C8869DBB4EDDD308F43C1A974A20A725A36EACCA123862FDE9945BF9D3E09", NULL},
        {"node.tox.biribiri.org",      33445, "F404ABAA1C99A9D37D61AB54898F56793E1DEF8BD46B1038B9D822E8460FAB67", NULL}
    };

    for (size_t i = 0; i < sizeof(nodes)/sizeof(DHT_node); i ++) {
        nodes[i].key_bin = hex2bin(nodes[i].key_hex);
        tox_bootstrap(tox, nodes[i].ip, nodes[i].port, nodes[i].key_bin, NULL);
    }
}

void friend_request_cb(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length,
                                   void *user_data)
{
    INFO("* receive friend request(use `/accept` to see).");

    struct Request *req = malloc(sizeof(struct Request));

    req->id = 1 + ((requests != NULL) ? requests->id : 0);
    req->is_friend_request = true;
    memcpy(req->userdata.friend.pubkey, public_key, TOX_PUBLIC_KEY_SIZE);
    req->msg = malloc(length + 1);
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

    if (friend_number*TALK_TYPE_COUNT + TALK_TYPE_FRIEND == TalkingTo) {
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
        sprintf(f->name, "%.*s", (int)length, (char*)name);
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
        req->userdata.conference.cookie = malloc(length);
        memcpy(req->userdata.conference.cookie, cookie, length),
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
        sprintf(cf->title, "%.*s", (int)length, (char*)title);
    }
}

void conference_message_cb(Tox *tox, uint32_t conference_number, uint32_t peer_number, TOX_MESSAGE_TYPE type, const uint8_t *message, size_t length, void *user_data) {
    struct Conference *cf = getconfer(conference_number);
    if (!cf) return;

    if (tox_conference_peer_number_is_ours(tox, conference_number, peer_number, NULL))  return;

    if (conference_number * TALK_TYPE_COUNT + TALK_TYPE_CONFERENCE == TalkingTo) {
        if (type == TOX_MESSAGE_TYPE_NORMAL) {
            char *peername = get_confer_peername(conference_number, peer_number);
            PRINT(GUEST_MSG_PREFIX "%.*s", getftime(), peername, (int)length, (char*)message);
        } else {
            INFO("* receive UNSURPPORT message type");
        }
    } else {
        INFO("* receive CONFERENCE message from %s\n",cf->title);
    }
}

void conference_peer_list_changed_cb(Tox *tox, uint32_t conference_number, void *user_data) {
    struct Conference *cf = getconfer(conference_number);
    if (!cf) return;

    TOX_ERR_CONFERENCE_PEER_QUERY err;
    uint32_t count = tox_conference_peer_count(tox, conference_number, &err);
    if (err != TOX_ERR_CONFERENCE_PEER_QUERY_OK) {
        ERROR("get conference peer count failed, errcode:%d",err);
        return;
    }
    if (cf->peers) free(cf->peers);
    cf->peers = calloc(count, sizeof(struct ConferPeer));
    cf->peers_count = count;

    for (int i=0;i<count;i++) {
        struct ConferPeer *p = cf->peers + i;
        tox_conference_peer_get_name(tox, conference_number, i, (uint8_t*)p->name, NULL);
        tox_conference_peer_get_public_key(tox, conference_number, i, p->pubkey,NULL);
    }
}

void create_tox()
{
    struct Tox_Options *options = tox_options_new(NULL);
    tox_options_set_start_port(options, PORT_RANGE_START);
    tox_options_set_end_port(options, PORT_RANGE_END);

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

    tox_options_free(options);
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
    self.friend_number = TALK_TYPE_NULL;
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
    tox_callback_conference_peer_list_changed(tox, conference_peer_list_changed_cb);


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
    if (narg == 0) { // self
        PRINT("%-.12s:%s", "Name", self.name);

        uint32_t addr_size = tox_address_size();
        uint8_t tox_id_bin[addr_size];
        tox_self_get_address(tox, tox_id_bin);
        char *hex = bin2hex(tox_id_bin, sizeof(tox_id_bin));
        PRINT("%-.12s:%s","Tox ID", hex);
        free(hex);

        PRINT("%-.12s:%s", "Status Msg",self.status_message);
        PRINT("%-.12s:%s", "Network",connection_enum2text(self.connection));
    }
    else {
        int num = atoi(args[0]);
        if (num % TALK_TYPE_COUNT == TALK_TYPE_CONFERENCE) {
            struct Conference *cf = getconfer(num / TALK_TYPE_COUNT);
            if (!cf) {
                WARN("Invalid contact index");
                return;
            }
            PRINT("GROUP TITLE:\t%s",cf->title);
            PRINT("PEER COUNT:\t%zu", cf->peers_count);
            PRINT("Peers:");
            for (int i=0;i<cf->peers_count;i++){
                PRINT("\t%s",cf->peers[i].name);
            }
        }
    }
}

void command_setname_helper(int narg, char **args) {
    char *name = args[0];
    size_t len = strlen(name);
    tox_self_set_name(tox, (uint8_t*)name, strlen(name), NULL);

    RESIZE(self.name, self.name_sz, len);
    sprintf(self.name, "%.*s", (int)len, name);
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

    uint8_t *bin_id = hex2bin(hex_id);
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
    PRINT("#Friends(talk_num|friend_num|name|connection|status message):\n");
    for (;f != NULL; f = f->next) {
        PRINT("%3d  %3d  %15.15s  %10.10s  %s",f->friend_number * TALK_TYPE_COUNT + TALK_TYPE_FRIEND, f->friend_number, f->name, connection_enum2text(f->connection), f->status_message);
    }

    struct Conference *cf = conferences;
    PRINT("\n#Groups(talk_num|group_num|count of peers|name):\n");
    for (;cf != NULL; cf = cf->next) {
        PRINT("%3d  %3d  %10d  %s",cf->conference_number * TALK_TYPE_COUNT + TALK_TYPE_CONFERENCE, cf->conference_number, tox_conference_peer_count(tox, cf->conference_number, NULL), cf->title);
    }
}

void command_save_helper(int narg, char **args) {
    update_savedata_file(tox);
}

void command_go_helper(int narg, char **args) {
    if (narg == 0) {
        TalkingTo = TALK_TYPE_NULL;
        strcpy(async_repl->prompt, CMD_PROMPT);
        return;
    }
    uint32_t num = (uint32_t)atoi(args[0]);
    switch (num % TALK_TYPE_COUNT) {
        case TALK_TYPE_FRIEND: {
            uint32_t friend_num = num/TALK_TYPE_COUNT;
            struct Friend *f = getfriend(friend_num);
            if (!f) {
                ERROR("! friend not exist");
                return;
            }
            TalkingTo = num;
            sprintf(async_repl->prompt, FRIEND_TALK_PROMPT, f->name);
            INFO("* talk to friend: %s", f->name);
            break;
       }
        case TALK_TYPE_CONFERENCE: {
            uint32_t conference_num = num/TALK_TYPE_COUNT;
            struct Conference *cf = getconfer(conference_num);
            if (!cf) {
                ERROR("! conference not exist");
                return;
            }
            TalkingTo = num;
            sprintf(async_repl->prompt, CONFERENCE_TALK_PROMPT, cf->title);
            INFO("* talk to conference: %s", cf->title);
            break;
       }
    }
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
                    TOX_ERR_CONFERENCE_JOIN err;
                    uint32_t conference_number = tox_conference_join(tox, data->friend_number, data->cookie, data->length, &err);
                    if (err != TOX_ERR_CONFERENCE_JOIN_OK) {
                        ERROR("! join conference failed, errcode: %d", err);
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

void command_invite_helper(int narg, char **args) {
    uint32_t friend_number = atoi(args[0]);
    if (!getfriend(friend_number)) {
        ERROR("! invalid friend_number");
        return;
    }
    int err;
    uint32_t conference_number;
    if (narg == 1) {
        conference_number = tox_conference_new(tox, (TOX_ERR_CONFERENCE_NEW*)&err);
        if (err != TOX_ERR_CONFERENCE_NEW_OK) {
            ERROR("! create conference failed, errcode:%d", err);
            return;
        }
        addconfer(conference_number);
    } else {
        conference_number = atoi(args[1]);
    }
    tox_conference_invite(tox, friend_number, conference_number, (TOX_ERR_CONFERENCE_INVITE*)&err);
    if (err != TOX_ERR_CONFERENCE_INVITE_OK) {
        ERROR("! conference invite failed, errcode:%d", err);
        return;
    }
}

void command_settitle_helper(int narg, char **args) {
    uint32_t conference_number = atoi(args[0]);
    char *title = args[1];
    struct Conference *cf = getconfer(conference_number);
    if (!cf) {
        ERROR("! Invalid group number");
        return;
    }
    
    TOX_ERR_CONFERENCE_TITLE  err;
    size_t len = strlen(title);
    tox_conference_set_title(tox, conference_number, (uint8_t*)title, len, &err);
    if (err != TOX_ERR_CONFERENCE_TITLE_OK) {
        ERROR("! set group title failed, errcode: %d",err);
    }

    RESIZE(cf->title, cf->title_sz, len);
    sprintf(cf->title, "%.*s",(int)len,title);
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
        0 + COMMAND_ARGS_REST,
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
        0 + COMMAND_ARGS_REST,
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
    {
        "invite",
        "<friend_number> [<group_number>]- invite a friend to a group chat.",
        1 + COMMAND_ARGS_REST,
        command_invite_helper,
    },
    {
        "settitle",
        "<group_number> <title> - set group title.",
        2,
        command_settitle_helper,
    },
};

void command_help_helper(int narg, char **args){
    for (int i=0;i<sizeof(commands)/sizeof(struct Command);i++) {
        PRINT("%-16s\t%s", commands[i].name, commands[i].desc);
    }
}

char *poptok(char **strp) {
    static const char *dem = " \t";
    char *save = *strp;
    *strp = strpbrk(*strp, dem);
    if (*strp == NULL) return save;

    *((*strp)++) = '\0';
    *strp += strspn(*strp,dem);
    return save;
}

void repl_iterate(){
    static char buf[128];
    static char line[LINE_MAX_SIZE];
    while (1) {
        int n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        for (int i=0;i<n;i++) { // for_1
            char c = buf[i];
            if (c == '\004')          /* C-d */
                exit(0);
            if (!arepl_readline(async_repl, c, line, sizeof(line))) continue; // continue to for_1

            int len = strlen(line);
            line[--len] = '\0'; // remove trailing \n

            if (TalkingTo != TALK_TYPE_NULL && line[0] != '/') {  // if talking to someone, just print the msg out.
                PRINT(SELF_MSG_PREFIX "%.*s", getftime(), self.name, len, line);
                switch (TalkingTo % TALK_TYPE_COUNT) {
                    case TALK_TYPE_FRIEND:
                        tox_friend_send_message(tox, TalkingTo/TALK_TYPE_COUNT, TOX_MESSAGE_TYPE_NORMAL, (uint8_t*)line, strlen(line), NULL);
                        continue; // continue to for_1
                    case TALK_TYPE_CONFERENCE:
                        tox_conference_send_message(tox, TalkingTo/TALK_TYPE_COUNT, TOX_MESSAGE_TYPE_NORMAL, (uint8_t*)line, strlen(line), NULL);
                        continue;  // continue to for_1
                }
            }

            PRINT(CMD_MSG_PREFIX "%s", line);

            if (line[0] == '/') {
                char *l = line + 1; // skip '/'
                char *cmdname = poptok(&l);
                struct Command *cmd = NULL;
                for (int j=0; j<COMMAND_LENGTH;j++){ // for_2
                    if (strcmp(commands[j].name, cmdname) == 0) {
                        cmd = &commands[j];
                        break; // break for_2
                    }
                }
                if (cmd) {
                    char *tokens[cmd->narg];
                    int ntok = 0;
                    for (; l != NULL && ntok != cmd->narg; ntok++) {  
                        // if it's the last arg, then take the rest line.
                        char *tok = (ntok == cmd->narg - 1) ? l : poptok(&l);
                        tokens[ntok] = tok;
                    }
                    if (ntok < cmd->narg - (cmd->narg >= COMMAND_ARGS_REST ? COMMAND_ARGS_REST : 0)) {
                        ERROR("! wrong number of cmd args");
                    } else {
                        cmd->handler(ntok, tokens);
                    }
                    continue; // continue to for_1
                }
            }

            WARN("Invalid command, try `/help` instead.");
        } // end for_1
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
