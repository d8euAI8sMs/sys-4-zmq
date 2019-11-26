#include "targetver.h"

#include <stdio.h>
#include <tchar.h>

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <zmq.h>

/* ***************************************************** */

struct config_t
{
    std::string src_host;
    std::string src_port;
    std::string dst_host;
    std::string dst_port;
    std::string profile;
};

using args_t = std::unordered_map < std::string, std::string > ;

args_t parse_cmd(int argc, char* argv[])
{
    args_t args;

    std::string last_option;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (0 == arg.compare(0, 2, "--")) // is option
        {
            arg = arg.substr(2);
            last_option = arg;
            args[arg] = "";
        }
        else if (last_option.empty())
        {
            // allow one single arg without mapped key
            args[""] = arg;
            continue;
        }
        else
        {
            args[last_option] = arg;
            last_option = "";
        }
    }

    return args;
}

std::string or_default(const args_t & args,
                       const std::string key,
                       const std::string & val)
{
    auto it = args.find(key);
    if (it != std::end(args)) return it->second;
    return val;
}

config_t make_config(const args_t & args)
{
    return {
        or_default(args, "src-host", "localhost"),
        or_default(args, "src-port", "5555"),
        or_default(args, "dst-host", "localhost"),
        or_default(args, "dst-port", "5557"),
        or_default(args, "", "worker"),
    };
}

/* ***************************************************** */

int source(const config_t & cfg)
{
    void * context = zmq_ctx_new();
    void * worker_pool = zmq_socket(context, ZMQ_PUSH);

    auto addr = "tcp://*:" + cfg.src_port;
    zmq_bind(worker_pool, addr.c_str());

    const int n_req = 100;
    const int sleep_ms = 500;
    for (int i = 0; i != n_req; ++i)
    {
        std::string buf = "hello - " + std::to_string(i + 1);
        printf("sending '%s'...\n", buf.c_str());
        zmq_send(worker_pool, buf.c_str(), buf.size(), 0);
        Sleep(sleep_ms);
    }

    zmq_close (worker_pool);
    zmq_ctx_destroy (context);
    return 0;
}

/* ***************************************************** */

int worker_stub(const config_t & cfg)
{
    void *context = zmq_ctx_new();
    void *source = zmq_socket(context, ZMQ_PULL);
    void *sink = zmq_socket(context, ZMQ_PUSH);

    auto src_addr = "tcp://" + cfg.src_host + ":" + cfg.src_port;
    zmq_connect(source, src_addr.c_str());
    
    auto dst_addr = "tcp://" + cfg.dst_host + ":" + cfg.dst_port;
    zmq_connect(sink, dst_addr.c_str());

    const int sleep_ms_min = 500;
    const int sleep_ms_std = 1000;
    while (true)
    {
        std::string buf(100, '\0');
        zmq_recv(source, (void*)buf.data(), buf.size(), 0);
        buf.resize(strlen(buf.c_str())); // compact buffer

        printf("received '%s'\n", buf.c_str());

        printf("  doing costly operation on string\n");
        Sleep(sleep_ms_min + (rand() % sleep_ms_std));
        std::transform(buf.begin(), buf.end(), buf.begin(),
                       [](unsigned char c){ return std::toupper(c); });
        printf("  end doing costly operation\n");

        printf("  sending '%s'\n", buf.c_str());
        zmq_send(sink, (void*)buf.data(), buf.size(), 0);
    }
    return 0;
}

/* ***************************************************** */

int sink_stub(const config_t & cfg)
{
    void *context = zmq_ctx_new();
    void *sink = zmq_socket(context, ZMQ_PULL);

    auto dst_addr = "tcp://*:" + cfg.dst_port;
    zmq_bind(sink, dst_addr.c_str());

    while (true)
    {
        std::string buf(100, '\0');
        zmq_recv(sink, (void*)buf.data(), buf.size(), 0);
        buf.resize(strlen(buf.c_str())); // compact buffer

        printf("received '%s'\n", buf.c_str());
    }
    return 0;
}

/* ***************************************************** */

int main(int argc, char* argv[])
{
    auto cfg = make_config(parse_cmd(argc, argv));
    printf("profile: %s\n", cfg.profile.c_str());

    if      ("source" == cfg.profile) return source(cfg);
    else if ("worker" == cfg.profile) return worker_stub(cfg);
    else if ("sink"   == cfg.profile) return sink_stub(cfg);

    printf("unknown profile\n");
    return -1;
}
