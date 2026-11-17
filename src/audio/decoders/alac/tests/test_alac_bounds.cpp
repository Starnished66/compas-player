#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include "EndianPortable.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int main() {
    for (unsigned channels = 1; channels <= 2; ++channels) {
        ALACSpecificConfig cookie = {};
        cookie.frameLength = Swap32NtoB(16); cookie.bitDepth = 16;
        cookie.numChannels = channels; cookie.sampleRate = Swap32NtoB(44100);
        ALACDecoder decoder; assert(decoder.Init(&cookie, sizeof(cookie)) == 0);
        unsigned char packet[256] = {}, output[128] = {};
        BitBuffer bits; BitBufferInit(&bits, packet, sizeof(packet));
        BitBufferWrite(&bits, channels == 1 ? 0 : 1, 3);
        BitBufferWrite(&bits, 0, 4); BitBufferWrite(&bits, 0, 12);
        BitBufferWrite(&bits, 9, 4); // partial, no shifted bits, uncompressed
        BitBufferWrite(&bits, 32, 32);
        BitBufferInit(&bits, packet, sizeof(packet));
        uint32_t decoded = 0;
        assert(decoder.Decode(&bits, output, 16, channels, &decoded) != 0);
        BitBufferInit(&bits, packet, sizeof(packet));
        assert(decoder.Decode(&bits, output, 32, channels, &decoded) != 0);
    }
    puts("ALAC oversized mono/stereo partial frames: rejected");
}
