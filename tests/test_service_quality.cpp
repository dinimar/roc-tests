#include <gtest/gtest.h>

#include <roc/context.h>
#include <roc/receiver.h>
#include <roc/sender.h>

#include <algorithm>
#include <future>
#include <vector>

typedef std::vector<std::vector<float>> FrameBuffer;

class ServiceQuality : public ::testing::Test {
protected:
    void SetUp() override {
        // create context
        roc_context_config context_config {};

        context_ = roc_context_open(&context_config);
        ASSERT_NE(context_, nullptr);

        // create receiver
        roc_receiver_config receiver_config {};
        receiver_config.frame_sample_rate = 44100;
        receiver_config.frame_channels = ROC_CHANNEL_SET_STEREO;
        receiver_config.frame_encoding = ROC_FRAME_ENCODING_PCM_FLOAT;
        receiver_config.automatic_timing = 1;
        receiver_config.resampler_profile = ROC_RESAMPLER_DISABLE;

        receiver_ = roc_receiver_open(context_, &receiver_config);
        ASSERT_NE(receiver_, nullptr);

        // bind receiver to two ports
        roc_address source_addr;
        ASSERT_EQ(roc_address_init(&source_addr, ROC_AF_AUTO, "127.0.0.1", 0), 0);

        ASSERT_EQ(roc_receiver_bind(receiver_, ROC_PORT_AUDIO_SOURCE,
                                    ROC_PROTO_RTP_RS8M_SOURCE, &source_addr),
                  0);

        roc_address repair_addr;
        ASSERT_EQ(roc_address_init(&repair_addr, ROC_AF_AUTO, "127.0.0.1", 0), 0);

        ASSERT_EQ(roc_receiver_bind(receiver_, ROC_PORT_AUDIO_REPAIR,
                                    ROC_PROTO_RS8M_REPAIR, &repair_addr),
                  0);

        // create sender
        roc_sender_config sender_config {};
        sender_config.frame_sample_rate = 44100;
        sender_config.frame_channels = ROC_CHANNEL_SET_STEREO;
        sender_config.frame_encoding = ROC_FRAME_ENCODING_PCM_FLOAT;
        sender_config.automatic_timing = 1;
        sender_config.resampler_profile = ROC_RESAMPLER_DISABLE;
        sender_config.fec_code = ROC_FEC_RS8M;

        sender_ = roc_sender_open(context_, &sender_config);
        ASSERT_NE(sender_, nullptr);

        // bind sender to a port
        roc_address sender_addr;
        ASSERT_EQ(roc_address_init(&sender_addr, ROC_AF_AUTO, "127.0.0.1", 0), 0);

        ASSERT_EQ(roc_sender_bind(sender_, &sender_addr), 0);

        // connect sender to receiver ports
        ASSERT_EQ(roc_sender_connect(sender_, ROC_PORT_AUDIO_SOURCE,
                                     ROC_PROTO_RTP_RS8M_SOURCE, &source_addr),
                  0);

        ASSERT_EQ(roc_sender_connect(sender_, ROC_PORT_AUDIO_REPAIR,
                                     ROC_PROTO_RS8M_REPAIR, &repair_addr),
                  0);
    }

    void TearDown() override {
        if (sender_) {
            EXPECT_EQ(roc_sender_close(sender_), 0);
        }

        if (receiver_) {
            EXPECT_EQ(roc_receiver_close(receiver_), 0);
        }

        if (context_) {
            EXPECT_EQ(roc_context_close(context_), 0);
        }
    }

    void Send(std::vector<float> send_frame, size_t num_frames) {
        // init buffer
        send_buffer.resize(num_frames);        

        for (size_t n = 0; n < num_frames; n++) {
            roc_frame frame = {};
            frame.samples = &send_frame[0];
            frame.samples_size = send_frame.size() * sizeof(float);

            ASSERT_EQ(roc_sender_write(sender_, &frame), 0);
            // save sent frame to buffer    
            send_buffer[n] = send_frame;
        }
    }

    void Receive(size_t frame_size, size_t num_frames) {
        std::vector<float> recv_frame(frame_size);
        bool received_something = false;
        // init buffer
        recv_buffer.resize(num_frames);

        for (size_t n = 0; n < num_frames; n++) {
            std::fill(recv_frame.begin(), recv_frame.end(), 0);

            roc_frame frame = {};
            frame.samples = &recv_frame[0];
            frame.samples_size = recv_frame.size() * sizeof(float);

            ASSERT_EQ(roc_receiver_read(receiver_, &frame), 0);
            // save received frame to buffer
            recv_buffer[n] = recv_frame;

            if (!std::all_of(recv_frame.begin(), recv_frame.end(),
                             [](float f) { return f == 0; })) {
                received_something = true;
            }
        }

        // This is a very basic check that we have recevied at least one non-zero sample.
        // Instead of it, we should generate some sequence on sender (e.g. a sine wave),
        // and on receiver check that we received the same sequence, probably with leading
        // and trailing zeros.
        EXPECT_TRUE(received_something);
    }

    roc_context* context_ {};
    roc_receiver* receiver_ {};
    roc_sender* sender_ {};
    FrameBuffer send_buffer {1}; // buffer for sent frames
    FrameBuffer recv_buffer {1}; // buffer for received frames
};

TEST_F(ServiceQuality, NoLoss) {
    size_t frame_size = 256;
    size_t num_frames = 500;
    std::vector<float> send_frames(frame_size);
    std::fill(send_frames.begin(), send_frames.end(), 0.5);

    auto sending = std::async(std::launch::async, [=]() { Send(send_frames, num_frames); });
    auto receiving = std::async(std::launch::async, [=](){ Receive(frame_size, num_frames); });
    // wait until all threads finish execution
    sending.wait();
    receiving.wait();
    
    // Check recv_buffer on broken frames
    size_t k = 0;
    for(size_t i=0; i<num_frames; i++) {
        for(size_t j=0; j<frame_size; j++) {
            if (recv_buffer[i][j] == 0) {
                k++;
            }
        }
        // If we've not got any correct float from frame
        if (k == frame_size) {
            std::cout << "All-zero Frame " << i << std::endl; 
            k = 0;
        } else if (k != frame_size && k != 0) {
            std::cout << "Frame " << i << "contains " << k << " zeros." << std::endl;
        }   
    }
}
