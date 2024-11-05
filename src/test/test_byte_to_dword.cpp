#include <radray/utility.h>

int main() {
    // 00000111 00001010 00001101 00001000
    // 00000001 00000000 00000000 00000000
    radray::uint8_t a[] = {0b111, 0b1010, 0b1101, 0b1000, 0b1};
    radray::vector<uint32_t> dwords = radray::ByteToDWORD(a);
    {
        uint32_t r = 0b00001000'00001101'00001010'00000111;
        if (dwords[0] != r) {
            std::abort();
        }
    }
    {
        uint32_t r = 0b00000001;
        if (dwords[1] != r) {
            std::abort();
        }
    }
    return 0;
}
