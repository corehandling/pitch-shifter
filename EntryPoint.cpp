#include <iostream>
#include <vector>
#include <portaudio.h>
#include <SoundTouchDLL.h>

#define FRAMES_PER_BUFFER 512

struct CallbackData
{
    HANDLE hST;            // SoundTouch instance
    PaSampleFormat format; // paFloat32 or paInt16
    int channels;
};

static int audioCallback(const void *input,
                         void *output,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo *,
                         PaStreamCallbackFlags,
                         void *userData)
{
    auto *data = (CallbackData *)userData;
    HANDLE h = data->hST;
    int channels = data->channels;

    // Temporary buffer in float for SoundTouch processing
    std::vector<float> inFloat(framesPerBuffer * channels);
    std::vector<float> outFloat(framesPerBuffer * channels);

    // --- Convert input to float ---
    if (data->format == paFloat32)
    {
        const float *in = (const float *)input;
        if (in)
        {
            std::copy(in, in + framesPerBuffer * channels, inFloat.begin());
        }
    }
    else if (data->format == paInt16)
    {
        const int16_t *in = (const int16_t *)input;
        if (in)
        {
            for (unsigned i = 0; i < framesPerBuffer * channels; i++)
            {
                inFloat[i] = in[i] / 32768.0f; // normalize to [-1,1]
            }
        }
    }

    // --- Feed into SoundTouch ---
    if (!inFloat.empty())
        soundtouch_putSamples(h, inFloat.data(), framesPerBuffer);

    // --- Receive processed samples ---
    int received = soundtouch_receiveSamples(h, outFloat.data(), framesPerBuffer);

    // Pad with silence if not enough
    for (int i = received * channels; i < (int)(framesPerBuffer * channels); i++)
    {
        outFloat[i] = 0.0f;
    }

    // --- Convert float back to output format ---
    if (data->format == paFloat32)
    {
        float *out = (float *)output;
        std::copy(outFloat.begin(), outFloat.end(), out);
    }
    else if (data->format == paInt16)
    {
        int16_t *out = (int16_t *)output;
        for (unsigned i = 0; i < framesPerBuffer * channels; i++)
        {
            float sample = outFloat[i];
            if (sample > 1.0f)
                sample = 1.0f;
            if (sample < -1.0f)
                sample = -1.0f;
            out[i] = (int16_t)(sample * 32767.0f);
        }
    }

    return paContinue;
}

int main()
{
    PaError err = Pa_Initialize();
    if (err != paNoError)
    {
        std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << "\n";
        return -1;
    }

    // ---- List Devices ----
    int numDevices = Pa_GetDeviceCount();
    if (numDevices < 0)
    {
        std::cerr << "ERROR: " << Pa_GetErrorText(numDevices) << "\n";
        Pa_Terminate();
        return -1;
    }

    for (int i = 0; i < numDevices; i++)
    {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        const PaHostApiInfo *host = Pa_GetHostApiInfo(info->hostApi);

        std::cout << "ID " << i
                  << " | Name: " << info->name
                  << " | Host API: " << host->name
                  << " | Max Input: " << info->maxInputChannels
                  << " | Max Output: " << info->maxOutputChannels
                  << " | Default SampleRate: " << info->defaultSampleRate
                  << "\n";
    }

    int inputDevice, outputDevice;
    std::cout << "\nSelect input device ID: ";
    std::cin >> inputDevice;
    std::cout << "Select output device ID: ";
    std::cin >> outputDevice;

    if (inputDevice < 0 || inputDevice >= numDevices ||
        outputDevice < 0 || outputDevice >= numDevices)
    {
        std::cerr << "Invalid device ID.\n";
        Pa_Terminate();
        return -1;
    }

    const PaDeviceInfo *inInfo = Pa_GetDeviceInfo(inputDevice);
    const PaDeviceInfo *outInfo = Pa_GetDeviceInfo(outputDevice);

    // Pick sample rate that both devices can handle
    double sampleRate = std::min(inInfo->defaultSampleRate, outInfo->defaultSampleRate);

    // Channels (pick max 2, but at least 1)
    int inChannels = (inInfo->maxInputChannels >= 2 ? 2 : (inInfo->maxInputChannels > 0 ? 1 : 0));
    int outChannels = (outInfo->maxOutputChannels >= 2 ? 2 : (outInfo->maxOutputChannels > 0 ? 1 : 0));

    if (inChannels == 0 || outChannels == 0)
    {
        std::cerr << "Selected devices do not support required I/O.\n";
        Pa_Terminate();
        return -1;
    }

    // Stream parameters
    PaStreamParameters inputParams;
    inputParams.device = inputDevice;
    inputParams.channelCount = inChannels;
    inputParams.suggestedLatency = inInfo->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters outputParams;
    outputParams.device = outputDevice;
    outputParams.channelCount = outChannels;
    outputParams.suggestedLatency = outInfo->defaultLowOutputLatency;
    outputParams.hostApiSpecificStreamInfo = nullptr;

    // SoundTouch setup
    HANDLE hST = soundtouch_createInstance();
    soundtouch_setSampleRate(hST, (unsigned int)sampleRate);
    soundtouch_setChannels(hST, inChannels); // assume same for input/output
    soundtouch_setPitchSemiTones(hST, -4.0f);

    // ---- Try float32 first ----
    PaSampleFormat fmt = paFloat32;
    inputParams.sampleFormat = fmt;
    outputParams.sampleFormat = fmt;

    CallbackData cbData{hST, fmt, inChannels};

    PaStream *stream = nullptr;
    err = Pa_OpenStream(&stream,
                        &inputParams,
                        &outputParams,
                        sampleRate,
                        FRAMES_PER_BUFFER,
                        paNoFlag,
                        audioCallback,
                        &cbData);

    // ---- Retry with int16 if float fails ----
    if (err == paSampleFormatNotSupported)
    {
        std::cerr << "Float32 not supported, retrying with Int16...\n";
        fmt = paInt16;
        inputParams.sampleFormat = fmt;
        outputParams.sampleFormat = fmt;
        cbData.format = fmt;

        err = Pa_OpenStream(&stream,
                            &inputParams,
                            &outputParams,
                            sampleRate,
                            FRAMES_PER_BUFFER,
                            paNoFlag,
                            audioCallback,
                            &cbData);
    }

    if (err != paNoError)
    {
        std::cerr << "Failed to open stream: " << Pa_GetErrorText(err) << "\n";
        soundtouch_destroyInstance(hST);
        Pa_Terminate();
        return -1;
    }

    std::cout << "Stream opened successfully. Starting...\n";
    Pa_StartStream(stream);

    std::cout << "Press ENTER to stop.\n";
    std::cin.ignore();
    std::cin.get();

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    soundtouch_destroyInstance(hST);
    Pa_Terminate();

    return 0;
}