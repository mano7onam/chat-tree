#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <cassert>
#include <vector>
#include <string>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <netdb.h>
#include <chrono>

typedef std::pair<uint32_t, unsigned short> Ip_Port;

//enum MSG_TYPE {CHILD, MSG, LEFT, PARENT, ROOT, GOODMSG};

const int GOODMSG = 0;
const int MSG = 1;
const int LEFT = 2;
const int PARENT = 3;
const int ROOT = 4;
const int CHILD = 5;

const int BUFFER_SIZE = 1000000;
const long long TIME_TO_KEEP = 5000000LL; // keep message 5 seconds
const long long TIME_KEEP_RECEIVED = 5000000LL;
const long long SENDING_INTERVAL = 1000000LL; // send messages not more than once every this interval
const int MESSAGE_MAX_SIZE = 100000;
const int ADDRESS_MAX_SIZE = 1000;

int socket_fd;
unsigned short my_port;
struct sockaddr_in my_addr;
bool flag_root = true;
int precent_lose = 50;

void * buf; // buffer to receive messages
int bsz; // size buffer

std::string tosend_message; // for MSG
std::string tosend_address; // for send address
int tosend_id_message; // for GOODMSG

// connections and
// messages map, pair - id receiver(first), number message - second
std::map<int, struct sockaddr_in> connections; // id -> address
std::map<Ip_Port, int> idconnections;
std::map<std::pair<int, int>, std::pair<void*, int>> MM; // messages map
std::map<std::pair<int, int>, long long> MT; // map times keep
std::map<std::pair<int, int>, long long> MST; // map times last send
std::map<int, std::map<int, long long>> received_messages; // map ids received messages
std::map<int, int> ids_msgs;
std::set<int> children;
int id_parent;
int global_map_id = 0;

void init_globals() {
    flag_root = true;
    global_map_id = 0;
    buf = malloc(BUFFER_SIZE);
}

void final_function() {
    free(buf);
}

std::string pack_address(struct sockaddr_in addr) {
    char str_addr[ADDRESS_MAX_SIZE];
    uint32_t ip = ntohl(addr.sin_addr.s_addr);
    unsigned short port = ntohs(addr.sin_port);
    sprintf(str_addr, "%d.%d.%d.%d:%d",
            (ip & 0xff000000) >> 24,
            (ip & 0x00ff0000) >> 16,
            (ip & 0x0000ff00) >> 8,
            (ip & 0x000000ff), port);

    return std::string(str_addr);
}

int get_id_new_message(int id_destination) {
    if (!ids_msgs.count(id_destination)) {
        ids_msgs[id_destination] = 0;
    }
    int res = ids_msgs[id_destination];
    ids_msgs[id_destination]++;

    return res;
}

int create_new_message(int type, int id_destination) {
    int id_new_message = get_id_new_message(id_destination);

    fprintf(stderr, "Create new: %d %d\n", type, id_destination);

    int size = 0;
    void * new_buf = malloc(BUFFER_SIZE);
    ((int*)new_buf)[0] = htonl((uint32_t)type);
    ((int*)new_buf)[1] = htonl((uint32_t)id_new_message);
    size += 2 * sizeof(uint32_t);

    if (type == MSG) {
        ((int*)new_buf)[2] = htonl((uint32_t)tosend_message.size());
        size += sizeof(int);
        memcpy((void*)((char*)new_buf + size), tosend_message.c_str(), tosend_message.size());
        size += tosend_message.size();
    }
    else if (type == PARENT || type == CHILD || type == LEFT) {
        ((int*)new_buf)[2] = htonl((uint32_t)tosend_address.size());
        fprintf(stderr, "Tosend: %s\n", tosend_address.c_str());
        size += sizeof(int);
        memcpy((void*)((char*)new_buf + size), tosend_address.c_str(), tosend_address.size());
        size += tosend_address.size();
    }

    auto key = std::make_pair(id_destination, id_new_message);
    MM[key] = {new_buf, size};
    auto cur_time = std::chrono::high_resolution_clock::now().time_since_epoch();
    MT[key] = std::chrono::duration_cast<std::chrono::microseconds>(cur_time).count();
    MST[key] = MT[{id_destination, id_new_message}] - SENDING_INTERVAL - 1;

    return id_new_message;
}

void send_message(int id_destination, int id_message) {
    std::pair<void*, int> send_buf = MM[{id_destination, id_message}];
    //fprintf(stderr, "Send message: %d %d\n", id_destination, id_message);
    //fprintf(stderr, "Type: %d\n", ntohl(((uint32_t*)send_buf.first)[0]));
    ssize_t size = sendto(socket_fd, send_buf.first, (size_t)send_buf.second, 0,
                          (struct sockaddr *)&connections[id_destination], sizeof(struct sockaddr_in));
    if (size < 0) {
        perror("Send");
        return;
    }
    std::string str_addr = pack_address(connections[id_destination]);
}

void send_good_message(int id_destination, int id_taken_message) {
    void* cur_buf = malloc(16);
    ((int*)cur_buf)[0] = htonl((uint32_t)GOODMSG);
    ((int*)cur_buf)[1] = htonl((uint32_t)id_taken_message);
    sendto(socket_fd, cur_buf, 2 * sizeof(int), 0,
           (struct sockaddr *)&connections[id_destination], sizeof(struct sockaddr_in));
    free(cur_buf);
}

void erase_message_from_maps(std::pair<int, int> key) {
    free(MM[key].first);
    MM.erase(key);
    MT.erase(key);
    MST.erase(key);
}

void delete_delivered_message(int id_who, int id_delivered_message) {
    auto key = std::make_pair(id_who, id_delivered_message);
    erase_message_from_maps(key);
}

void check_and_resend(long long current_time) {
    std::vector<std::pair<int, int>> outdates_messages;
    for (auto elem : MT) {
        long long diff = current_time - elem.second;
        if (diff > TIME_TO_KEEP) {
            outdates_messages.push_back(elem.first);
        }
    }

    for (auto key : outdates_messages) {
        erase_message_from_maps(key);
    }

    for (auto elem : MM) {
        long long diff = current_time - MST[elem.first];
        if (diff > SENDING_INTERVAL) {
            int id_destination = elem.first.first;
            int id_message = elem.first.second;
            send_message(id_destination, id_message);
            MST[elem.first] = current_time;
        }
    }

    for (auto it = received_messages.begin(); it != received_messages.end(); ++it) {
        std::vector<int> outdates_received_kept;
        for (auto msg : it->second) {
            long long diff = current_time - msg.second;
            if (diff > TIME_KEEP_RECEIVED) {
                outdates_received_kept.push_back(msg.first);
            }
        }
        for (auto id : outdates_received_kept) {
            it->second.erase(id);
        }
    }
}

void on_signal_close(int sig) {
    fprintf(stderr, "Call exit function\n");

    if (!flag_root) {
        tosend_address = pack_address(my_addr);
        create_new_message(LEFT, id_parent);
    }

    fprintf(stderr, "Children size: %ld\n", children.size());
    int sz = (int)children.size();
    if (sz) {
        int new_parent;
        if (flag_root) {
            new_parent = *children.begin();
            create_new_message(ROOT, new_parent);
        }
        else {
            new_parent = id_parent;
        }
        for (auto ch : children) {
            if (ch != new_parent) {
                tosend_address = pack_address(connections[new_parent]);
                create_new_message(PARENT, ch);
                tosend_address = pack_address(connections[ch]);
                create_new_message(CHILD, new_parent);
            }
        }
    }

    while (!MT.empty()) {
        auto cur_time = std::chrono::high_resolution_clock::now().time_since_epoch();
        check_and_resend(std::chrono::duration_cast<std::chrono::microseconds>(cur_time).count());
    }

    exit(0);
}

Ip_Port get_ip_port(struct sockaddr_in addr) {
    uint32_t ip = addr.sin_addr.s_addr;
    unsigned short port = addr.sin_port;
    return std::make_pair(ip, port);
}

struct sockaddr_in get_address(char *strip, char *strport) {
    struct sockaddr_in addr;
    bzero(&addr, sizeof(struct sockaddr_in));
    addr.sin_family = PF_INET;
    addr.sin_port = htons((unsigned short)atoi(strport));
    struct hostent *host_info = gethostbyname(strip);
    memcpy(&(addr.sin_addr), host_info->h_addr, (size_t)host_info->h_length);
    return addr;
}

bool parse_address(char* cstr_addr, struct sockaddr_in &addr) {
    std::string str_addr(cstr_addr);
    size_t pos = str_addr.find(':');
    size_t len = str_addr.size();
    if (pos == std::string::npos) {
        fprintf(stderr, "Bad string\n");
        return false;
    }

    char strip[1000];
    char strport[1000];
    fprintf(stderr, "%ld %ld %ld\n", pos, pos + 1, len - pos - 1);
    strncpy(strip, cstr_addr, pos);
    strip[pos] = '\0';
    strncpy(strport, cstr_addr + pos + 1, len - pos - 1);
    strport[len - pos - 1] = '\0';

    addr = get_address(strip, strport);
    return true;
}

bool parse_parent_address(char* address) {
    parse_address(address, connections[global_map_id]);
    idconnections[get_ip_port(connections[global_map_id])] = global_map_id;
    id_parent = global_map_id;
    ++global_map_id;

    return true;
};

void do_express_myself() {
    fprintf(stderr, "I am ");

    std::string str_addr = pack_address(my_addr);
    fprintf(stderr, "%s\n", str_addr.c_str());

    if (!flag_root) {
        tosend_address = pack_address(my_addr);
        create_new_message(CHILD, id_parent);
    }
}

void read_message_from_console() {
    char message[MESSAGE_MAX_SIZE];
    fgets(message, MESSAGE_MAX_SIZE, stdin);
    tosend_message = std::string(message);
    fprintf(stderr, "Input from console: %s\n", message);

    if (!flag_root) {
        create_new_message(MSG, id_parent);
    }
    for (auto ch : children) {
        create_new_message(MSG, ch);
    }
}

void handle_child_message(struct sockaddr_in addr_from) {
    size_t size_addr = ntohl(((uint32_t*)buf)[2]);
    char str_addr[ADDRESS_MAX_SIZE];
    strncpy(str_addr, (char*)buf + 3 * sizeof(int), size_addr);

    struct sockaddr_in addr;
    if (str_addr[0] == '0') {
        addr = addr_from;
    }
    else {
        str_addr[size_addr] = '\0';
        parse_address(str_addr, addr);
    }
    fprintf(stderr, "Receive child with address: %s\n", pack_address(addr).c_str());

    Ip_Port ip_port = get_ip_port(addr);
    if (!idconnections.count(ip_port)) {
        idconnections[ip_port] = global_map_id;
        connections[global_map_id] = addr;
        ++global_map_id;
    }

    int id_chld = idconnections[ip_port];
    if (!children.count(id_chld)) {
        fprintf(stderr, "Insert this new child to set\n");
        children.insert(id_chld);
    }
}

void handle_parent_message(struct sockaddr_in addr_from) {
    size_t size_addr = ntohl(((uint32_t*)buf)[2]);
    char str_addr[ADDRESS_MAX_SIZE];
    strncpy(str_addr, (char*)buf + 3 * sizeof(int), size_addr);

    struct sockaddr_in addr;
    if (str_addr[0] == '0') {
        addr = addr_from;
    }
    else {
        str_addr[size_addr] = '\0';
        parse_address(str_addr, addr);
    }

    Ip_Port ip_port = get_ip_port(addr);
    if (!idconnections.count(ip_port)) {
        idconnections[ip_port] = global_map_id;
        connections[global_map_id] = addr;
        ++global_map_id;
    }

    id_parent = idconnections[ip_port];
}

void handle_left(struct sockaddr_in addr_from) {
    size_t size_addr = ntohl(((uint32_t*)buf)[2]);
    char str_addr[ADDRESS_MAX_SIZE];
    strncpy(str_addr, (char*)buf + 3 * sizeof(int), size_addr);

    struct sockaddr_in addr;
    if (str_addr[0] == '0') {
        addr = addr_from;
    }
    else {
        str_addr[size_addr] = '\0';
        parse_address(str_addr, addr);
    }

    Ip_Port ip_port = get_ip_port(addr);
    if (idconnections.count(ip_port)) {
        int id = idconnections[ip_port];
        children.erase(id);
    }
    else {
        fprintf(stderr, "Already left\n");
    }
}

int handle_message(struct sockaddr_in addr, int from_id) {
    // if only on children or parent
    if (!children.count(from_id) && (flag_root || !flag_root && from_id != id_parent)) {
        return 0;
    }
    // ttl on new connector

    std::string str_addr = pack_address(addr);
    fprintf(stderr, "Message from: %s\n", str_addr.c_str());

    char message[MESSAGE_MAX_SIZE];
    size_t size_message = ntohl(((uint32_t*)buf)[2]);
    strncpy(message, (char*)buf + 3 * sizeof(int), size_message);
    message[size_message] = '\0';
    fprintf(stderr, "%s\n", message);

    tosend_message = std::string(message);
    if (!flag_root && from_id != id_parent) {
        fprintf(stderr, "Vajno!!! %d %d\n", from_id, id_parent);
        fprintf(stderr, "Сhildren size: %ld\n", children.size());
        create_new_message(MSG, id_parent);
    }
    else {
        fprintf(stderr, "No resend parent\n");
    }
    for (auto ch : children) {
        if (ch != from_id) {
            create_new_message(MSG, ch);
        }
    }
    return 1;
}

int recv_message() {
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(struct sockaddr_in);
    ssize_t size = recvfrom(socket_fd, buf, BUFFER_SIZE, 0, (struct sockaddr *) &addr, &addr_size);

    if (rand() % 99 + 1 < precent_lose) {
        fprintf(stderr, "Bwahaha you just lose new message: %d\n", precent_lose);
        return 0;
    }

    Ip_Port ip_port = get_ip_port(addr);
    int current_id;
    if (!idconnections.count(ip_port)) {
        idconnections[ip_port] = global_map_id;
        connections[global_map_id] = addr;
        current_id = global_map_id;
        ++global_map_id;
    }
    else {
        current_id = idconnections[ip_port];
    }

    if (size < 4) {
        fprintf(stderr, "Bad message size\n");
        return -1;
    }

    int type = (int)ntohl(((uint32_t*)buf)[0]);
    int id_message = (int)ntohl(((uint32_t*)buf)[1]);
    fprintf(stderr, "Type message: %d\n", type);

    if (received_messages[current_id].count(id_message)) {
        return 0;
    }
    else {
        auto cur_time = std::chrono::high_resolution_clock::now().time_since_epoch();
        long long ll_cur_time = std::chrono::duration_cast<std::chrono::microseconds>(cur_time).count();
        received_messages[current_id][id_message] = ll_cur_time;
    }

    if (GOODMSG == type) {
        delete_delivered_message(current_id, id_message);
        return 1;
    }

    if (CHILD == type) {
        handle_child_message(addr);
        send_good_message(current_id, id_message);
    }
    else if (PARENT == type) {
        handle_parent_message(addr);
        send_good_message(current_id, id_message);
    }
    else if (ROOT == type) {
        flag_root = true;
        fprintf(stderr, "!!! I am a root now\n");
        send_good_message(current_id, id_message);
    }
    else if (LEFT == type) {
        handle_left(addr);
        send_good_message(current_id, id_message);
    }
    else if (MSG == type) {
        int res = handle_message(addr, current_id);
        if (1 == res) {
            send_good_message(current_id, id_message);
        }
    }

    return 1;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, on_signal_close);
    signal(SIGTERM, on_signal_close);

    init_globals();

    int opt = 0;
    while ((opt = getopt(argc, argv, "p:l:m:")) != -1) {
        switch (opt) {
            case 'p':
                fprintf(stderr, "CCCCCCCCCCCCCCCCCCCCCCCC\n");
                flag_root = false;
                parse_parent_address(optarg);
                break;
            case 'l':
                precent_lose = atoi(optarg);
                break;
            case 'm':
                my_port = (unsigned short)atoi(optarg);
                break;
            default:
                fprintf(stderr, "Unknown argument\n");
                exit(EXIT_FAILURE);
        }
    }
    if (!my_port) {
        fprintf(stderr, "No port\n");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "AAAAAAAAAAAAAAAAAAAAAAAAA\n");

    socket_fd = socket(PF_INET, SOCK_DGRAM, 0);
    int broadcastEnable = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
    bzero(&my_addr, sizeof(struct sockaddr_in));
    my_addr.sin_family = PF_INET;
    my_addr.sin_port = htons(my_port);
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr_in)) < 0) {
        perror("Bind");
        exit(EXIT_FAILURE);
    }

    do_express_myself();

    bool flag_main_cycle = true;
    while (flag_main_cycle) {
        fd_set fds;
        int max_fd = std::max(fileno(stdin), socket_fd);
        FD_ZERO(&fds);
        FD_SET(socket_fd, &fds);
        FD_SET(fileno(stdin), &fds);

        struct timeval tv = {0, SENDING_INTERVAL / 2};
        int activity = select(max_fd + 1, &fds, 0, 0, &tv);
        if (activity < 0) {
            continue;
        }
        /*if (activity) {
            fprintf(stderr, "Activity: %d\n", activity);
        }*/

        auto cur_time = std::chrono::high_resolution_clock::now().time_since_epoch();
        check_and_resend(std::chrono::duration_cast<std::chrono::microseconds>(cur_time).count());

        if (FD_ISSET(fileno(stdin), &fds)) {
            read_message_from_console();
        }

        if (FD_ISSET(socket_fd, &fds)) {
            //fprintf(stderr, "!!! On socket\n");
            recv_message();
        }
        //recv_message();
    }

    final_function();
    return 0;
}
