#ifndef MOTION_DATASOURCE_CPP
#define MOTION_DATASOURCE_CPP

#include <Motion/DataSource.h>
#include <Motion/DataSource.hpp>

constexpr std::size_t MAX_AUDIO_SAMPLES = 192000;
constexpr std::size_t PACKET_QUEUE_AMOUNT = 5;
constexpr AVRational TIME_BASE_Q = { 1, AV_TIME_BASE };

// Hack to deal with VS2015 and the 'awesome'... "Universal CRT"
#if WIN32	 && _MSC_VER > 1300

FILE _iob[] = { *stdin, *stdout, *stderr };
extern "C" FILE * __cdecl __iob_func(void) { return _iob; }

#endif

namespace mt
{
    DataSource::DataSource() :
        m_updateClock(),
        m_playingOffset(),
        m_fileLength(sf::seconds(-1)),
        m_videoSize(-1, -1),
        m_audioChannelCount(-1),
        m_playbackSpeed(1),
        m_videoStreamID(-1),
        m_audioStreamID(-1),
        m_formatContext(nullptr),
        m_videoContext(nullptr),
        m_audioContext(nullptr),
        m_videoCodec(nullptr),
        m_audioCodec(nullptr),
        m_videoRawFrame(nullptr),
        m_videoRGBAFrame(nullptr),
        m_audioRawBuffer(nullptr),
        m_videoRawBuffer(nullptr),
        m_videoRGBABuffer(nullptr),
        m_audioPCMBuffer(nullptr),
        m_videoSWContext(nullptr),
        m_audioSWContext(nullptr),
        m_state(State::Stopped),
        m_decodeThread(nullptr),
        m_shouldThreadRun(false),
        m_EOFReached(false),
        m_PlayingToEOF(false),
        m_playbackMutex(),
        m_videoPlaybacks(),
        m_audioPlaybacks()
    {
        av_register_all();
    }

    DataSource::~DataSource()
    {
        Cleanup();
        {
            sf::Lock lock(m_playbackMutex);

            while (m_videoPlaybacks.size() > 0)
            {
                m_videoPlaybacks.back()->m_dataSource = nullptr;
                m_videoPlaybacks.pop_back();
            }

            while (m_audioPlaybacks.size() > 0)
            {
                m_audioPlaybacks.back()->m_dataSource = nullptr;
                m_audioPlaybacks.pop_back();
            }
        }
    }

    void DataSource::Cleanup()
    {
        Stop();
        StopDecodeThread();

        m_videoStreamID = -1;
        m_audioStreamID = -1;
        m_playingOffset = sf::Time::Zero;
        m_fileLength = sf::seconds(-1);
        m_videoSize = sf::Vector2i(-1, -1);
        m_audioChannelCount = -1;

        if (m_videoContext)
        {
            avcodec_close(m_videoContext);
            m_videoContext = nullptr;
        }

        m_videoCodec = nullptr;

        if (m_audioContext)
        {
            avcodec_close(m_audioContext);
            m_audioContext = nullptr;
        }

        m_audioCodec = nullptr;

        if (m_videoRawFrame) DestroyPictureFrame(m_videoRawFrame, m_videoRawBuffer);
        if (m_videoRGBAFrame) DestroyPictureFrame(m_videoRGBAFrame, m_videoRGBABuffer);

        if (m_audioRawBuffer)
        {
            av_frame_free(&m_audioRawBuffer);
            m_audioRawBuffer = nullptr;
        }

        if (m_audioPCMBuffer)
        {
            av_free(m_audioPCMBuffer);
            m_audioPCMBuffer = nullptr;
        }

        if (m_videoSWContext)
        {
            sws_freeContext(m_videoSWContext);
            m_videoSWContext = nullptr;
        }

        if (m_audioSWContext)
        {
            swr_free(&m_audioSWContext);
            m_audioSWContext = nullptr;
        }

        if (m_formatContext)
        {
            avformat_close_input(&m_formatContext);
            m_formatContext = nullptr;
        }
    }

    bool DataSource::LoadFromFile(const std::string& Filename, bool EnableVideo, bool EnableAudio)
    {
        Cleanup();

        if (avformat_open_input(&m_formatContext, Filename.c_str(), nullptr, nullptr) != 0)
        {
            std::cout << "Motion: Failed to open file: '" << Filename << "'" << std::endl;
            return false;
        }

        if (avformat_find_stream_info(m_formatContext, nullptr) < 0)
        {
            std::cout << "Motion: Failed to find stream information" << std::endl;
            return false;
        }

        for (unsigned int i = 0; i < m_formatContext->nb_streams; i++)
        {
            switch (m_formatContext->streams[i]->codec->codec_type)
            {
                case AVMEDIA_TYPE_VIDEO:
                    if (m_videoStreamID == -1 && EnableVideo) m_videoStreamID = i;
                    break;

                case AVMEDIA_TYPE_AUDIO:
                    if (m_audioStreamID == -1 && EnableAudio) m_audioStreamID = i;
                    break;

                default:
                    break;
            }
        }

        if (HasVideo())
        {
            m_videoContext = m_formatContext->streams[m_videoStreamID]->codec;

            if (!m_videoContext)
            {
                std::cout << "Motion: Failed to get video codec context" << std::endl;
                m_videoStreamID = -1;
            }
            else
            {
                m_videoCodec = avcodec_find_decoder(m_videoContext->codec_id);

                if (!m_videoCodec)
                {
                    std::cout << "Motion: Failed to find video codec" << std::endl;
                    m_videoStreamID = -1;
                }
                else
                {
                    if (avcodec_open2(m_videoContext, m_videoCodec, nullptr) != 0)
                    {
                        std::cout << "Motion: Failed to load video codec" << std::endl;
                        m_videoStreamID = -1;
                    }
                    else
                    {
                        m_videoSize = sf::Vector2i(m_videoContext->width, m_videoContext->height);
                        m_videoRawFrame = CreatePictureFrame(m_videoContext->pix_fmt, m_videoSize.x, m_videoSize.y, m_videoRawBuffer);
                        m_videoRGBAFrame = CreatePictureFrame(AVPixelFormat::AV_PIX_FMT_BGRA, m_videoSize.x, m_videoSize.y, m_videoRGBABuffer);

                        if (!m_videoRawFrame || !m_videoRGBAFrame)
                        {
                            std::cout << "Motion: Failed to create video frames" << std::endl;
                            m_videoStreamID = -1;
                        }
                        else
                        {
                            int swapmode = SWS_FAST_BILINEAR;
                            if (m_videoSize.x * m_videoSize.y <= 500000 && m_videoSize.x % 8 != 0) swapmode |= SWS_ACCURATE_RND;
                            m_videoSWContext = sws_getCachedContext(nullptr, m_videoSize.x, m_videoSize.y, m_videoContext->pix_fmt, m_videoSize.x, m_videoSize.y, AVPixelFormat::AV_PIX_FMT_RGBA, swapmode, nullptr, nullptr, nullptr);
                        }
                    }
                }
            }
        }

        if (HasAudio())
        {
            m_audioContext = m_formatContext->streams[m_audioStreamID]->codec;

            if (!m_audioContext)
            {
                std::cout << "Motion: Failed to get audio codec context" << std::endl;
                m_audioStreamID = -1;
            }
            else
            {
                m_audioCodec = avcodec_find_decoder(m_audioContext->codec_id);

                if (!m_audioCodec)
                {
                    std::cout << "Motion: Failed to find audio codec" << std::endl;
                    m_audioStreamID = -1;
                }
                else
                {
                    if (avcodec_open2(m_audioContext, m_audioCodec, nullptr) != 0)
                    {
                        std::cout << "Motion: Failed to load audio codec" << std::endl;
                        m_audioStreamID = -1;
                    }
                    else
                    {
                        m_audioRawBuffer = av_frame_alloc();

                        if (!m_audioRawBuffer)
                        {
                            std::cout << "Motion: Failed to allocate audio buffer" << std::endl;
                            m_audioStreamID = -1;
                        }
                        else
                        {
                            if (av_samples_alloc(&m_audioPCMBuffer, nullptr, m_audioContext->channels, av_samples_get_buffer_size(nullptr, m_audioContext->channels, MAX_AUDIO_SAMPLES, AV_SAMPLE_FMT_S16, 0), AV_SAMPLE_FMT_S16, 0) < 0)
                            {
                                std::cout << "Motion: Failed to create audio samples buffer" << std::endl;
                                m_audioStreamID = -1;
                            }
                            else
                            {
                                av_frame_unref(m_audioRawBuffer);

                                uint64_t inchanlayout = m_audioContext->channel_layout;
                                uint64_t outchanlayout = inchanlayout;

                                if (inchanlayout == 0)
                                    inchanlayout = av_get_default_channel_layout(m_audioContext->channels);

                                if (outchanlayout != AV_CH_LAYOUT_MONO)
                                    outchanlayout = AV_CH_LAYOUT_STEREO;

                                m_audioSWContext = swr_alloc();

                                av_opt_set_int(m_audioSWContext, "in_channel_layout", inchanlayout, 0);
                                av_opt_set_int(m_audioSWContext, "out_channel_layout", outchanlayout, 0);
                                av_opt_set_int(m_audioSWContext, "in_sample_rate", m_audioContext->sample_rate, 0);
                                av_opt_set_int(m_audioSWContext, "out_sample_rate", m_audioContext->sample_rate, 0);
                                av_opt_set_sample_fmt(m_audioSWContext, "in_sample_fmt", m_audioContext->sample_fmt, 0);
                                av_opt_set_sample_fmt(m_audioSWContext, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

                                swr_init(m_audioSWContext);

                                m_audioChannelCount = av_get_channel_layout_nb_channels(outchanlayout);
                            }
                        }
                    }
                }
            }
        }

        if (m_formatContext->duration != AV_NOPTS_VALUE)
            m_fileLength = sf::milliseconds(static_cast<int>(m_formatContext->duration) / 1000);

        if (HasVideo() || HasAudio())
        {
            SetPlayingOffset(sf::Time::Zero);
            StartDecodeThread();

            sf::Lock lock(m_playbackMutex);

            for (auto& videoplayback : m_videoPlaybacks)
            {
                videoplayback->SourceReloaded();
            }

            for (auto& audioplayback : m_audioPlaybacks)
            {
                audioplayback->SourceReloaded();
            }

            return true;
        }
        else
        {
            std::cout << "Motion: Failed to load audio or video" << std::endl;
            Cleanup();

            return false;
        }
    }

    bool DataSource::HasVideo() const
    {
        return m_videoStreamID != -1;
    }

    bool DataSource::HasAudio() const
    {
        return m_audioStreamID != -1;
    }

    sf::Vector2i DataSource::GetVideoSize() const
    {
        return m_videoSize;
    }

    State DataSource::GetState() const
    {
        return m_state;
    }

    sf::Time DataSource::GetVideoFrameTime() const
    {
        if (!HasVideo())
            return sf::Time::Zero;

        AVRational r1 = m_formatContext->streams[m_videoStreamID]->avg_frame_rate;
        AVRational r2 = m_formatContext->streams[m_videoStreamID]->r_frame_rate;

        if ((!r1.num || !r1.den) && (!r2.num || !r2.den))
            return sf::seconds(1.0f / 29.97f);
        else
        {
            if (r2.num && r1.den)
                return sf::seconds(1.0f / ((float)r1.num / (float)r1.den));
            else
                return sf::seconds(1.0f / ((float)r2.num / (float)r2.den));
        }
    }

    int DataSource::GetAudioChannelCount() const
    {
        return m_audioChannelCount;
    }

    int DataSource::GetAudioSampleRate() const
    {
        if (!HasAudio())
            return -1;

        return m_audioContext->sample_rate;
    }

    void DataSource::Play()
    {
        if ((HasVideo() || HasAudio()) && m_state != State::Playing)
        {
            m_EOFReached = false;

            m_updateClock.restart();

            NotifyStateChanged(State::Playing);
        }
    }

    void DataSource::Pause()
    {
        if (m_state == State::Playing)
            NotifyStateChanged(State::Paused);
    }

    void DataSource::Stop()
    {
        if (m_state != State::Stopped)
        {
            NotifyStateChanged(State::Stopped);
            SetPlayingOffset(sf::Time::Zero);
        }
    }

    sf::Time DataSource::GetFileLength() const
    {
        return m_fileLength;
    }

    sf::Time DataSource::GetPlayingOffset() const
    {
        return m_playingOffset;
    }

    void DataSource::SetPlayingOffset(sf::Time PlayingOffset)
    {
        if (HasVideo() || HasAudio())
        {
            bool startThread = m_shouldThreadRun;

            if (startThread)
                StopDecodeThread();

            m_playingOffset = PlayingOffset;
            m_PlayingToEOF = false;

            bool startplaying = m_state == State::Playing;

            if (m_state != State::Stopped)
                NotifyStateChanged(State::Stopped);

            int64_t seekTarget = static_cast<int64_t>(PlayingOffset.asSeconds() * static_cast<float>(AV_TIME_BASE));

            if (HasAudio())
            {
                AVRational timebase = m_formatContext->streams[m_audioStreamID]->time_base;

                if (timebase.den != 0)
                    seekTarget = av_rescale_q(seekTarget, TIME_BASE_Q, timebase);

                avformat_seek_file(m_formatContext, m_audioStreamID, INT64_MIN, seekTarget, INT64_MAX, 0);
            }
            else if (HasVideo())
            {
                AVRational timebase = m_formatContext->streams[m_videoStreamID]->time_base;

                if (timebase.den != 0)
                    seekTarget = av_rescale_q(seekTarget, TIME_BASE_Q, timebase);

                avformat_seek_file(m_formatContext, m_videoStreamID, INT64_MIN, seekTarget, INT64_MAX, 0);
            }

            if (HasAudio())
                avcodec_flush_buffers(m_audioContext);

            if (HasVideo())
                avcodec_flush_buffers(m_videoContext);

            if (startThread)
                StartDecodeThread();

            if (startplaying)
                Play();
        }
    }

    void DataSource::NotifyStateChanged(State NewState)
    {
        sf::Lock lock(m_playbackMutex);

        if (NewState == State::Stopped)
            m_EOFReached = true;

        for (auto& videoplayback : m_videoPlaybacks)
        {
            videoplayback->StateChanged(m_state, NewState);
        }

        for (auto& audioplayback : m_audioPlaybacks)
        {
            audioplayback->StateChanged(m_state, NewState);
        }

        if (NewState == State::Stopped)
            m_EOFReached = false;

        m_state = NewState;
    }

    void DataSource::Update()
    {
        if (m_playingOffset > m_fileLength)
        {
            Stop();
            m_EOFReached = true;
        }

        sf::Time deltatime = m_updateClock.restart() * m_playbackSpeed;

        if (m_state == State::Playing)
            m_playingOffset += deltatime;

        sf::Lock lock(m_playbackMutex);

        for (auto& videoplayback : m_videoPlaybacks)
        {
            videoplayback->Update(deltatime);
        }

        for (auto& audioplayback : m_audioPlaybacks)
        {
            audioplayback->Update(deltatime);
        }
    }

    float DataSource::GetPlaybackSpeed() const
    {
        return m_playbackSpeed;
    }

    void DataSource::SetPlaybackSpeed(float PlaybackSpeed)
    {
        m_playbackSpeed = PlaybackSpeed;

        sf::Lock lock(m_playbackMutex);

        for (auto& audioplayback : m_audioPlaybacks)
        {
            audioplayback->SetPlaybackSpeed(PlaybackSpeed);
        }
    }

    void DataSource::StartDecodeThread()
    {
        if (m_shouldThreadRun)
            return;

        m_shouldThreadRun = true;

        m_decodeThread.reset(new std::thread(&DataSource::DecodeThreadRun, this));
    }

    void DataSource::StopDecodeThread()
    {
        if (!m_shouldThreadRun)
            return;

        m_shouldThreadRun = false;

        if (m_decodeThread->joinable())
            m_decodeThread->join();

        m_decodeThread.reset(nullptr);
    }

    void DataSource::DecodeThreadRun()
    {
        while (m_shouldThreadRun)
        {
            bool isfull = IsFull();

            while (!isfull && m_shouldThreadRun && !m_PlayingToEOF)
            {
                bool validpacket = false;

                while (!validpacket && m_shouldThreadRun)
                {
                    AVPacket* packet = (AVPacket*)av_malloc(sizeof(*packet));

                    av_init_packet(packet);

                    if (av_read_frame(m_formatContext, packet) == 0)
                    {
                        if (packet->stream_index == m_videoStreamID)
                        {
                            int decoderesult = 0;

                            if (avcodec_decode_video2(m_videoContext, m_videoRawFrame, &decoderesult, packet) >= 0)
                            {
                                if (decoderesult)
                                {
                                    if (sws_scale(m_videoSWContext, m_videoRawFrame->data, m_videoRawFrame->linesize, 0, m_videoContext->height, m_videoRGBAFrame->data, m_videoRGBAFrame->linesize))
                                    {
                                        validpacket = true;

                                        priv::VideoPacketPtr packet(std::make_shared<priv::VideoPacket>(m_videoRGBAFrame->data[0], m_videoSize.x, m_videoSize.y));
                                        {
                                            sf::Lock lock(m_playbackMutex);

                                            for (auto& videoplayback : m_videoPlaybacks)
                                            {
                                                sf::Lock playbacklock(videoplayback->m_protectionMutex);

                                                videoplayback->m_queuedVideoPackets.push(packet);
                                            }
                                        }

                                        isfull = IsFull();
                                    }
                                }
                            }
                        }
                        else if (packet->stream_index == m_audioStreamID)
                        {
                            int decoderesult = 0;

                            if (avcodec_decode_audio4(m_audioContext, m_audioRawBuffer, &decoderesult, packet) > 0)
                            {
                                if (decoderesult)
                                {
                                    int convertlength = swr_convert(m_audioSWContext, &m_audioPCMBuffer, m_audioRawBuffer->nb_samples, (const uint8_t**)m_audioRawBuffer->extended_data, m_audioRawBuffer->nb_samples);

                                    if (convertlength > 0)
                                    {
                                        validpacket = true;

                                        priv::AudioPacketPtr packet(std::make_shared<priv::AudioPacket>(m_audioPCMBuffer, convertlength, m_audioChannelCount));
                                        {
                                            sf::Lock lock(m_playbackMutex);

                                            for (auto& audioplayback : m_audioPlaybacks)
                                            {
                                                sf::Lock playbacklock(audioplayback->m_protectionMutex);

                                                audioplayback->m_queuedAudioPackets.push(packet);
                                            }
                                        }

                                        isfull = IsFull();
                                    }
                                }
                            }
                        }

                        av_free_packet(packet);
                        av_free(packet);
                    }
                    else
                    {
                        m_PlayingToEOF = true;
                        validpacket = true;

                        av_free_packet(packet);
                        av_free(packet);
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    bool DataSource::IsFull()
    {
        sf::Lock lock(m_playbackMutex);

        if (HasVideo())
        {
            for (auto& videoplayback : m_videoPlaybacks)
            {
                sf::Lock playbacklock(videoplayback->m_protectionMutex);

                if (videoplayback->m_queuedVideoPackets.size() < PACKET_QUEUE_AMOUNT)
                    return false;
            }
        }
        else if (HasAudio())
        {
            for (auto& audioplayback : m_audioPlaybacks)
            {
                sf::Lock playbacklock(audioplayback->m_protectionMutex);

                if (audioplayback->m_queuedAudioPackets.size() < PACKET_QUEUE_AMOUNT)
                    return false;
            }
        }

        return true;
    }

    AVFrame* DataSource::CreatePictureFrame(AVPixelFormat SelectedPixelFormat, int Width, int Height, uint8_t*& PictureBuffer)
    {
        AVFrame* picture = av_frame_alloc();

        if (!picture)
            return nullptr;

        int size = avpicture_get_size(SelectedPixelFormat, Width, Height);

        PictureBuffer = (uint8_t*)av_malloc(size);

        if (!PictureBuffer)
        {
            av_frame_free(&picture);
            return nullptr;
        }

        avpicture_fill((AVPicture*)picture, PictureBuffer, SelectedPixelFormat, Width, Height);

        return picture;
    }

    void DataSource::DestroyPictureFrame(AVFrame*& PictureFrame, uint8_t*& PictureBuffer)
    {
        av_free(PictureBuffer);
        av_frame_free(&PictureFrame);

        PictureBuffer = nullptr;
        PictureFrame = nullptr;
    }

    bool DataSource::IsEndofFileReached() const
    {
        return m_EOFReached;
    }
}

mtDataSource* mtDataSource_Create(void)
{
    mtDataSource* datasource = new mtDataSource();
    datasource->Value = new mt::DataSource();

    return datasource;
}

void mtDataSource_Destroy(mtDataSource* DataSource)
{
    delete DataSource->Value;
    delete DataSource;
}

sfBool mtDataSource_LoadFromFile(mtDataSource* DataSource, const char* Filename, sfBool EnableVideo, sfBool EnableAudio)
{
    return DataSource->Value->LoadFromFile(Filename, EnableVideo == sfTrue, EnableAudio == sfTrue);
}

void mtDataSource_Play(mtDataSource* DataSource)
{
    DataSource->Value->Play();
}

void mtDataSource_Pause(mtDataSource* DataSource)
{
    DataSource->Value->Pause();
}

void mtDataSource_Stop(mtDataSource* DataSource)
{
    DataSource->Value->Stop();
}

sfBool mtDataSource_HasVideo(mtDataSource* DataSource)
{
    return DataSource->Value->HasVideo();
}

sfBool mtDataSource_HasAudio(mtDataSource* DataSource)
{
    return DataSource->Value->HasAudio();
}

sfVector2i mtDataSource_GetVideoSize(mtDataSource* DataSource)
{
    sf::Vector2i size = DataSource->Value->GetVideoSize();

    sfVector2i retval;
    retval.x = size.x;
    retval.y = size.y;

    return retval;
}

mtState mtDataSource_GetState(mtDataSource* DataSource)
{
    return static_cast<mtState>(DataSource->Value->GetState());
}

sfTime mtDataSource_GetVideoFrameTime(mtDataSource* DataSource)
{
    sfTime retval;

    retval.microseconds = DataSource->Value->GetVideoFrameTime().asMicroseconds();

    return retval;
}

int mtDataSource_GetAudioChannelCount(mtDataSource* DataSource)
{
    return DataSource->Value->GetAudioChannelCount();
}

int mtDataSource_GetAudioSampleRate(mtDataSource* DataSource)
{
    return DataSource->Value->GetAudioSampleRate();
}

sfTime mtDataSource_GetFileLength(mtDataSource* DataSource)
{
    sfTime retval;

    retval.microseconds = DataSource->Value->GetFileLength().asMicroseconds();

    return retval;
}

sfTime mtDataSource_GetPlayingOffset(mtDataSource* DataSource)
{
    sfTime retval;

    retval.microseconds = DataSource->Value->GetPlayingOffset().asMicroseconds();

    return retval;
}

void mtDataSource_SetPlayingOffset(mtDataSource* DataSource, sfTime PlayingOffset)
{
    DataSource->Value->SetPlayingOffset(sf::microseconds(PlayingOffset.microseconds));
}

void mtDataSource_Update(mtDataSource* DataSource)
{
    DataSource->Value->Update();
}

float mtDataSource_GetPlaybackSpeed(mtDataSource* DataSource)
{
    return DataSource->Value->GetPlaybackSpeed();
}

void mtDataSource_SetPlaybackSpeed(mtDataSource* DataSource, float PlaybackSpeed)
{
    DataSource->Value->SetPlaybackSpeed(PlaybackSpeed);
}

sfBool mtDataSource_GetIsEndofFileReached(mtDataSource* DataSource)
{
    return DataSource->Value->IsEndofFileReached();
}

#endif
