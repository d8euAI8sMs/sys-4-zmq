#include "targetver.h"

#include <stdio.h>
#include <tchar.h>

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <zmq.h>
#include <opencv/cv.hpp>

/* ***************************************************** */

struct config_t
{
    std::string src_host;
    std::string src_port;
    std::string dst_host;
    std::string dst_port;
    std::string profile;
    std::string realtime;
    std::string filter;
    std::string cam_url;
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
        or_default(args, "realtime", "false"),
        or_default(args, "filter", "false"),
        or_default(args, "cam_url", ".\\sample.mov"),
    };
}

void frame_encode(uint32_t seq, const std::vector < uint8_t > & buf, zmq_msg_t & msg)
{
    zmq_msg_init_size(&msg, sizeof(seq) + buf.size());
    *(uint32_t*)zmq_msg_data(&msg) = seq;
    memcpy_s((uint8_t*)zmq_msg_data(&msg) + sizeof(seq),
                buf.size(), buf.data(), buf.size());
}

void frame_decode(zmq_msg_t & msg, uint32_t & seq, std::vector < uint8_t > & buf)
{
    seq = *(uint32_t *)zmq_msg_data(&msg);
    buf.resize(zmq_msg_size(&msg) - sizeof(seq));
    memcpy_s(buf.data(), buf.size(),
             (uint8_t *)zmq_msg_data(&msg) + sizeof(seq), buf.size());
}

/* ***************************************************** */

int source(const config_t & cfg)
{
    cv::VideoCapture cap(cfg.cam_url);
    cv::Mat frame;
    std::vector < uint8_t > buf;

    void * context = zmq_ctx_new();
    void * worker_pool = zmq_socket(context, ZMQ_PUSH);

    auto addr = "tcp://*:" + cfg.src_port;
    zmq_bind(worker_pool, addr.c_str());

    uint32_t seq = 0;
    while (cap.read(frame))
    {
        // show frame
        cv::imshow("SOURCE", frame);
        cv::waitKey(1000 / 25); // ~25 fps

        printf("  captured '%d' frame\n", seq + 1);

        // encode frame
        cv::imencode(".jpg", frame, buf);

        // prepare message
        zmq_msg_t msg; frame_encode(seq, buf, msg);

        zmq_msg_send(&msg, worker_pool, 0);

        zmq_msg_close(&msg);

        ++seq;
    }

    zmq_close (worker_pool);
    zmq_ctx_destroy (context);
    return 0;
}

/* ***************************************************** */

void process_image(cv::Mat & in_out);

int worker_stub(const config_t & cfg)
{
    void *context = zmq_ctx_new();
    void *source = zmq_socket(context, ZMQ_PULL);
    void *sink = zmq_socket(context, ZMQ_PUSH);

    auto src_addr = "tcp://" + cfg.src_host + ":" + cfg.src_port;
    zmq_connect(source, src_addr.c_str());
    
    auto dst_addr = "tcp://" + cfg.dst_host + ":" + cfg.dst_port;
    zmq_connect(sink, dst_addr.c_str());

    std::vector < uint8_t > buf;
    cv::Mat frame;
    uint32_t seq;

    while (true)
    {
        zmq_msg_t msg;
        zmq_msg_init(&msg);

        zmq_msg_recv(&msg, source, 0);

        // drop all outstanding messages to always operate
        // on relatively fresh data (slow worker workaround)
        //
        // [don't put this before blocking recv - it may
        //  cause unwanted effect of the newest messages
        //  being dropped; this implementation automatically
        //  purges the queue and stores the latest message
        //  in the buffer]
        while (cfg.realtime != "false")
        {
            zmq_msg_t msg0;
            zmq_msg_init(&msg0);
            int res = zmq_msg_recv(&msg0, source, ZMQ_DONTWAIT);
            if ((res == -1) && (errno == EAGAIN)) break;
            printf("  *  dropping message  *\n");
            zmq_msg_close(&msg);
            msg = msg0;
        }

        frame_decode(msg, seq, buf);

        printf("  received '%d' frame\n", seq + 1);

        // decode, blur and send image to the sink
        cv::imdecode(buf, cv::IMREAD_COLOR, &frame);
        process_image(frame);
        cv::imencode(".jpg", frame, buf);

        frame_encode(seq, buf, msg);
        
        zmq_msg_send(&msg, sink, 0);

        zmq_msg_close(&msg);
    }
    return 0;
}

// stub image processing
void process_image(cv::Mat & in_out)
{
    cv::GaussianBlur(in_out, in_out, { 51, 51 }, 0);
}

/* ***************************************************** */

int sink_stub(const config_t & cfg)
{
    void *context = zmq_ctx_new();
    void *sink = zmq_socket(context, ZMQ_PULL);

    auto dst_addr = "tcp://*:" + cfg.dst_port;
    zmq_bind(sink, dst_addr.c_str());

    std::vector < uint8_t > buf;
    cv::Mat frame;
    uint32_t seq, last_seq = 0;

    while (true)
    {
        zmq_msg_t msg;
        zmq_msg_init(&msg);

        zmq_msg_recv(&msg, sink, 0);

        frame_decode(msg, seq, buf);

        printf("received '%d' frame\n", seq);

        // filter out outdated frames
        if ((cfg.filter != "false") && (seq < last_seq)) continue;

        // decode frame
        cv::imdecode(buf, cv::IMREAD_COLOR, &frame);

        // show frame
        cv::imshow("SOURCE", frame);
        cv::waitKey(1000 / 25); // ~25 fps

        zmq_msg_close(&msg);

        last_seq = seq;
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
