#include "targetver.h"

#include <stdio.h>
#include <tchar.h>

#include <string>
#include <array>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <zmq.h>
#include <opencv/cv.hpp>

#ifndef PACKET_VER
#define PACKET_VER 2
#endif

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

/* ***************************************************** */

struct packet_t
{
    uint32_t ver;
    uint32_t seq;
    std::vector < std::vector < uint8_t > > frms;
};

packet_t make_packet()
{
    return { PACKET_VER, 0 };
}

packet_t make_packet(uint32_t n)
{
    packet_t p = make_packet();
    p.frms.resize(n);
    return p;
}

bool validate_packet(const packet_t & p)
{
    return p.ver == PACKET_VER;
}

void frame_encode(const packet_t & p, zmq_msg_t & msg)
{
    uint32_t len;
    auto size = sizeof(p.ver) + sizeof(p.seq) + sizeof(len);
    for (size_t i = 0; i < p.frms.size(); ++i)
    {
        size += p.frms[i].size() + sizeof(len);
    }

    zmq_msg_init_size(&msg, size);

    auto data = (uint8_t*)zmq_msg_data(&msg);
    *(decltype(p.ver)*)(data + offsetof(packet_t, ver)) = p.ver;
    *(decltype(p.seq)*)(data + offsetof(packet_t, seq)) = p.seq;

    data += offsetof(packet_t, frms);

    for (size_t i = 0; i < p.frms.size(); ++i)
    {
        len = p.frms[i].size();
        *(decltype(len)*)(data) = len;

        memcpy_s(data + sizeof(len), len, p.frms[i].data(), len);
        data += len + sizeof(len);
    }

    *(decltype(len)*)(data) = 0;
}

bool frame_decode(zmq_msg_t & msg, packet_t & p)
{
    uint32_t len;

    auto data = (uint8_t*)zmq_msg_data(&msg);
    p.ver = *(decltype(p.ver)*)(data + offsetof(packet_t, ver));
    p.seq = *(decltype(p.seq)*)(data + offsetof(packet_t, seq));

    data += offsetof(packet_t, frms);

    p.frms.clear();

    while (true)
    {
        len = *(decltype(len)*)(data);
        if (len == 0) break;

        p.frms.emplace_back(len);
        memcpy_s(p.frms.back().data(), len, data + sizeof(len), len);
        data += len + sizeof(len);
    }

    return validate_packet(p);
}

/* ***************************************************** */

int source(const config_t & cfg)
{
    cv::VideoCapture cap(cfg.cam_url);
    cv::Mat frame;
    packet_t packet = make_packet(1);

    void * context = zmq_ctx_new();
    void * worker_pool = zmq_socket(context, ZMQ_PUSH);

    auto addr = "tcp://*:" + cfg.src_port;
    zmq_bind(worker_pool, addr.c_str());

    while (cap.read(frame))
    {
        // show frame
        cv::imshow("SOURCE", frame);
        cv::waitKey(1000 / 25); // ~25 fps

        printf("  captured '%d' frame\n", packet.seq + 1);

        // encode frame
        cv::imencode(".jpg", frame, packet.frms[0]);

        // prepare message
        zmq_msg_t msg; frame_encode(packet, msg);

        zmq_msg_send(&msg, worker_pool, 0);

        zmq_msg_close(&msg);

        ++packet.seq;
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

    cv::Mat frame;
    packet_t in_packet;
    packet_t out_packet = make_packet(2);

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

        if (frame_decode(msg, in_packet))
        {
            printf("  received '%d' frame\n", in_packet.seq + 1);

            // decode, blur and send image to the sink
            cv::imdecode(in_packet.frms[0], cv::IMREAD_COLOR, &frame);
            process_image(frame);
            cv::imencode(".jpg", frame, out_packet.frms[1]);

            out_packet.seq = in_packet.seq;
            out_packet.frms[0] = std::move(in_packet.frms[0]);

            frame_encode(out_packet, msg);
        
            zmq_msg_send(&msg, sink, 0);
        }
        else
        {
            printf("  invalid packet received\n");
        }

        zmq_msg_close(&msg);
    }
    return 0;
}

//finding boundaries
int border_image(cv::Mat& frame_open_CV, cv::Mat& detected_edges)
{
	int lowThreshold = 200;
	int ratio = 10;
	int kernel_size = 5;
	cv::Mat frame_open_CV_gray, dst;
	dst.create(frame_open_CV.size(), frame_open_CV.type());
	cv::cvtColor(frame_open_CV, frame_open_CV_gray, cv::COLOR_BGR2GRAY);
	cv::blur(frame_open_CV_gray, detected_edges, cv::Size(kernel_size, kernel_size));
	cv::Canny(detected_edges, detected_edges, lowThreshold, lowThreshold * ratio, kernel_size);
	dst = cv::Scalar::all(0);
	frame_open_CV.copyTo(dst, detected_edges);
	frame_open_CV_gray.release();
	dst.release();
	return 0;
}

//reduction and merge
int reduction_plus_merge_image(cv::Mat& frame_open_CV, cv::Mat& detected_edges, cv::Mat& reduction_matrix)
{
	cv::Mat channels_frame_open_CV[3], channels_detected_edges[3];
	int frame_CV_width = frame_open_CV.size().width;
	int frame_CV_height = frame_open_CV.size().height;
	cv::split(frame_open_CV, channels_frame_open_CV);
	cv::split(detected_edges, channels_detected_edges);
	int max_z = 256;
	int step_z = 64;
	for (int i = 0; i < frame_CV_height; i++)
	{
		for (int j = 0; j < frame_CV_width; j++)
		{
			for (uchar z = 0; z < max_z - step_z; z += step_z)
			{
				if (channels_frame_open_CV[0].data[i * frame_CV_width + j] > z&&
					channels_frame_open_CV[0].data[i * frame_CV_width + j] < z + step_z)
				{
					channels_frame_open_CV[0].data[i * frame_CV_width + j] = z + step_z / 2;
				}
				if (channels_frame_open_CV[1].data[i * frame_CV_width + j] > z&&
					channels_frame_open_CV[1].data[i * frame_CV_width + j] < z + step_z)
				{
					channels_frame_open_CV[1].data[i * frame_CV_width + j] = z + step_z / 2;
				}
				if (channels_frame_open_CV[2].data[i * frame_CV_width + j] > z&&
					channels_frame_open_CV[2].data[i * frame_CV_width + j] < z + step_z)
				{
					channels_frame_open_CV[2].data[i * frame_CV_width + j] = z + step_z / 2;
				}
				if (channels_detected_edges[0].data[i * frame_CV_width + j] == 255)
				{

					channels_frame_open_CV[0].data[i * frame_CV_width + j] = 0;
					channels_frame_open_CV[1].data[i * frame_CV_width + j] = 0;
					channels_frame_open_CV[2].data[i * frame_CV_width + j] = 255;
				}
			}
		}
	}
	cv::merge(channels_frame_open_CV, 3, reduction_matrix);
	channels_frame_open_CV->release();
	channels_detected_edges->release();
	return 0;
}

//image processing
int process_image(cv::Mat& in_out)
{
	cv::Mat frame_open_CV,
		detected_edges,
		reduction_matrix;
	in_out.copyTo(frame_open_CV);
	border_image(frame_open_CV, detected_edges);
	reduction_plus_merge_image(frame_open_CV, detected_edges, reduction_matrix);
	reduction_matrix.copyTo(in_out);
	in_out = cv::Mat();
	frame_open_CV.release();
	detected_edges.release();
	reduction_matrix.release();
	return 0;
}

/* ***************************************************** */

int sink_stub(const config_t & cfg)
{
    void *context = zmq_ctx_new();
    void *sink = zmq_socket(context, ZMQ_PULL);

    auto dst_addr = "tcp://*:" + cfg.dst_port;
    zmq_bind(sink, dst_addr.c_str());

    cv::Mat frame, orig, stack;
    uint32_t last_seq = 0;
    packet_t packet;

    while (true)
    {
        zmq_msg_t msg;
        zmq_msg_init(&msg);

        zmq_msg_recv(&msg, sink, 0);

        if (frame_decode(msg, packet))
        {
            printf("received '%d' frame\n", packet.seq);

            // filter out outdated frames
            if ((cfg.filter != "false") && (packet.seq < last_seq)) continue;

            // decode frames
            cv::imdecode(packet.frms[0], cv::IMREAD_COLOR, &orig);
            cv::imdecode(packet.frms[1], cv::IMREAD_COLOR, &frame);

            // stack original and processed frames
            cv::vconcat(orig, frame, stack);
            double c = 1.5 * 480.0 / stack.rows;
            cv::resize(stack, stack, {}, c, c);

            // show frame
            cv::imshow("SINK", stack);
            cv::waitKey(1000 / 25); // ~25 fps
        }
        else
        {
            printf("invalid packet received\n");
        }

        zmq_msg_close(&msg);

        last_seq = packet.seq;
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
