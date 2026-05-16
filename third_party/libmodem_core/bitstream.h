// **************************************************************** //
// libmodem - APRS modem                                            //
// Version 0.1.0                                                    //
// https://github.com/iontodirel/libmodem                           //
// Copyright (c) 2025 Ion Todirel                                   //
// **************************************************************** //
//
// bitstream.h
//
// MIT License
//
// Copyright (c) 2025 Ion Todirel
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <array>
#include <span>
#include <tuple>
#include <type_traits>

#ifndef LIBMODEM_AX25_NAMESPACE_BEGIN
#define LIBMODEM_AX25_NAMESPACE_BEGIN namespace ax25 {
#endif
#ifndef LIBMODEM_AX25_NAMESPACE_END
#define LIBMODEM_AX25_NAMESPACE_END }
#endif
#ifndef LIBMODEM_AX25_USING_NAMESPACE
#define LIBMODEM_AX25_USING_NAMESPACE using namespace ax25;
#endif
#ifndef LIBMODEM_AX25_NAMESPACE_REFERENCE
#define LIBMODEM_AX25_NAMESPACE_REFERENCE ax25 :: 
#endif
#ifndef LIBMODEM_FX25_NAMESPACE_BEGIN
#define LIBMODEM_FX25_NAMESPACE_BEGIN namespace fx25 {
#endif
#ifndef LIBMODEM_FX25_NAMESPACE_END
#define LIBMODEM_FX25_NAMESPACE_END }
#endif
#ifndef LIBMODEM_FX25_USING_NAMESPACE
#define LIBMODEM_FX25_USING_NAMESPACE using namespace fx25;
#endif
#ifndef LIBMODEM_FX25_NAMESPACE_REFERENCE
#define LIBMODEM_FX25_NAMESPACE_REFERENCE fx25 :: 
#endif
#ifndef LIBMODEM_IL2P_NAMESPACE_BEGIN
#define LIBMODEM_IL2P_NAMESPACE_BEGIN namespace il2p {
#endif
#ifndef LIBMODEM_IL2P_NAMESPACE_END
#define LIBMODEM_IL2P_NAMESPACE_END }
#endif
#ifndef LIBMODEM_IL2P_USING_NAMESPACE
#define LIBMODEM_IL2P_USING_NAMESPACE using namespace il2p;
#endif
#ifndef LIBMODEM_IL2P_NAMESPACE_REFERENCE
#define LIBMODEM_IL2P_NAMESPACE_REFERENCE il2p :: 
#endif

#ifndef LIBMODEM_NAMESPACE
#define LIBMODEM_NAMESPACE libmodem
#endif
#ifndef LIBMODEM_NAMESPACE_BEGIN
#define LIBMODEM_NAMESPACE_BEGIN namespace LIBMODEM_NAMESPACE {
#endif
#ifndef LIBMODEM_NAMESPACE_REFERENCE
#define LIBMODEM_NAMESPACE_REFERENCE libmodem :: 
#endif
#ifndef LIBMODEM_NAMESPACE_END
#define LIBMODEM_NAMESPACE_END }
#endif

#ifndef LIBMODEM_INLINE
#define LIBMODEM_INLINE inline
#endif

LIBMODEM_NAMESPACE_BEGIN

// **************************************************************** //
//                                                                  //
//                                                                  //
// address                                                          //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct address
{
    std::array<char, 6> text = {};
    size_t text_length = 0;
    int ssid = 0;
    bool mark = false; // H-bit
    bool command_response = false; // C/R-bit
    std::array<uint8_t, 2> reserved_bits = { 1, 1 };
};

bool try_parse_address(std::string_view address_string, struct address& address);
bool try_parse_address(std::string_view address, std::string& address_no_ssid, int& ssid);
bool try_parse_address_with_used_flag(std::string_view address, std::string& address_no_ssid, int& ssid, bool& mark);
std::string to_string(const struct address& address);
std::string to_string(const struct address& address, bool ignore_mark);
bool validate_address(const struct address& address);
bool try_parse_int(std::string_view string, int& value);

// **************************************************************** //
//                                                                  //
//                                                                  //
// packet                                                           //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct packet
{
    packet() = default;
    packet(const packet& other) = default;
    packet& operator=(const packet& other) = default;
    packet(const std::string& from, const std::string& to, const std::vector<std::string>& path, const std::string& data);
    packet(const char* packet_string);
    packet(const std::string& packet_string);
    operator std::string() const;

    std::string from;
    std::string to;
    std::vector<std::string> path;
    std::string data;
};

bool operator==(const packet& lhs, const packet& rhs);
std::string to_string(const packet& p);
bool try_decode_packet(std::string_view packet_string, packet& result);

// **************************************************************** //
//                                                                  //
//                                                                  //
// container_traits                                                 //
//                                                                  //
//                                                                  //
// **************************************************************** //

template<typename Container>
struct container_traits
{
    static void push_back(Container& c, uint8_t v) { c.push_back(v); }
    static auto begin(Container& c) { return c.begin(); }
    static auto end(Container& c) { return c.end(); }
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// traits_back_insert_iterator                                      //
//                                                                  //
//                                                                  //
// **************************************************************** //

template<class Container, class Traits>
class traits_back_insert_iterator
{
public:
    using iterator_category = std::output_iterator_tag;
    using difference_type = std::ptrdiff_t;

    explicit constexpr traits_back_insert_iterator(Container& c) noexcept : container(std::addressof(c))
    {
    }

    template<class V>
        requires std::constructible_from<typename Container::value_type, V>
    constexpr traits_back_insert_iterator& operator=(V&& v) noexcept(noexcept(Traits::push_back(*container, typename Container::value_type(std::forward<V>(v)))))
    {
        Traits::push_back(*container, typename Container::value_type(std::forward<V>(v)));
        return *this;
    }

    constexpr traits_back_insert_iterator& operator*() noexcept { return *this; }
    constexpr traits_back_insert_iterator& operator++() noexcept { return *this; }
    constexpr traits_back_insert_iterator  operator++(int) noexcept { return *this; }

private:
    Container* container;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// frame                                                            //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_AX25_NAMESPACE_BEGIN

enum class frame_type : uint8_t
{
    // I-frame
    i,

    // S-frames
    rr,
    rnr,
    rej,
    srej,

    // U-frames
    ui,
    sabm,
    sabme,
    disc,
    dm,
    ua,
    frmr,
    xid,
    test,

    unknown
};

frame_type get_frame_type(uint8_t control);
bool is_u_frame_type(frame_type type);
bool is_s_frame_type(frame_type type);

struct frame
{
    address from;
    address to;
    std::array<address, 8> path = {};
    size_t path_count = 0;
    std::array<uint8_t, 256> data = {};
    size_t data_length = 0;
    std::array<uint8_t, 2> crc = { 0, 0 };
    std::array<uint8_t, 2> control = { 0x03, 0x00 };
    uint8_t pid = 0xF0;
    bool has_pid = false;
    bool is_mod128 = false;
};

packet to_packet(const struct frame& frame);
frame to_frame(const packet& p);

bool validate_pid(uint8_t pid);
bool validate_frame(const struct frame& frame);

template<typename ForwardIt1, typename ForwardIt2>
bool validate_frame(const address& from, const address& to, ForwardIt1 path_first, ForwardIt1 path_last, ForwardIt2 data_first, ForwardIt2 data_last, uint8_t control, uint8_t pid, const std::array<uint8_t, 2>& actual_crc, const std::array<uint8_t, 2>& expected_crc);

LIBMODEM_AX25_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// bitstream_state                                                  //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_AX25_NAMESPACE_BEGIN

struct bitstream_state
{
    void reset();

    bool searching = true;        // Searching for preamble. Internal use.
    bool in_preamble = false;     // Currently in preamble. Internal use.
    bool in_frame = false;        // Currently in frame. Internal use.
    bool complete = false;        // Frame complete. Internal use.
    bool aborted = false;         // True if an abort sequence (7+ consecutive 1s) was detected in the frame
    uint8_t last_nrzi_level = 0;  // The last NRZI level seen. Internal use.
    size_t frame_start_index = 0; // Index in bitstream buffer where current frame bits start
    size_t frame_end_index = 0;   // Index in bitstream buffer where current frame bits end (excluding postamble)

    size_t bitstream_size = 0;    // Number of valid bits in external bitstream buffer. Internal use.

    uint8_t hdlc_shift_register = 0; // Rolling 8-bit register for HDLC flag detection. Internal use.

    // Incremental byte assembly state for try_decode_bitstream_bare. Internal use.
    uint8_t current_byte = 0;
    int bit_index = 0;
    int stuff_count = 0;
    size_t byte_count = 0;

    bool enable_diagnostics = false; // Enable diagnostic frame capture

    uint8_t frame_nrzi_level = 0;         // Initial NRZI level at the start of the frame, after the preamble. Requires diagnostics.
    uint8_t frame_nrzi_level_pending = 0; // Internal use
    uint8_t preamble_nrzi_level = 0;      // NRZI level before the preamble flags
    size_t frame_size_bits = 0;           // Size of the last decoded frame in bits, excluding any HDLC flags

    // Maximum allowed in-progress bitstream size (includes the 8-bit closing flag before it is recognized).
    // A valid frame of N payload bits reaches bitstream_size = N + 8 before completion,
    // so set this to (desired_max_payload_bits + 8)
    // 0 = use buffer capacity or default 8000.
    size_t max_frame_bits = 0;

    size_t preamble_count = 0;            // Number of preamble flags detected
    size_t postamble_count = 0;           // Number of postamble flags detected
    size_t preamble_count_pending = 0;    // Internal use
    size_t postamble_count_pending = 0;   // Internal use

    size_t global_preamble_start = 0;     // Global count of bits until the current frame start with HDLC flags, 1-based indexing
    size_t global_postamble_end = 0;      // Global count of bits until the current frame end with HDLC flags, 1-based indexing
    size_t global_bit_count = 0;          // Global bits processed
    size_t global_preamble_start_pending = 0; // Global bit position where current preamble started. Requires diagnostics.
};

LIBMODEM_AX25_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// ax25_bitstream_converter                                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct ax25_bitstream_converter
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags, int postamble_flags) const;
    bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read);
    bool try_decode(uint8_t bit, packet& p);
    void reset();

private:
    LIBMODEM_AX25_NAMESPACE_REFERENCE bitstream_state state;
    std::vector<uint8_t> bitstream_buffer;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// ax25_scrambled_bitstream_converter                               //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct ax25_scrambled_bitstream_converter
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags, int postamble_flags) const;
    bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read);
    bool try_decode(uint8_t bit, packet& p);
    void reset();
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// fx25_bitstream_converter                                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct fx25_bitstream_converter
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags, int postamble_flags) const;
    bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read);
    void reset();
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// fx25_scrambled_bitstream_converter                                //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct fx25_scrambled_bitstream_converter
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags, int postamble_flags) const;
    bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read);
    void reset();
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// il2p_bitstream_converter                                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct il2p_bitstream_converter
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f) const;
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const;
    std::vector<uint8_t> encode(const packet& p) const;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags, int postamble_flags) const;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// bitstream_converter_base                                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct bitstream_converter_base
{
    virtual std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const = 0;
    virtual std::vector<uint8_t> encode(const packet& p, int preamble_flags, int postamble_flags) const = 0;
    virtual bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read) = 0;
    virtual bool try_decode(uint8_t bit, packet& p) = 0;
    virtual void reset() = 0;
    virtual ~bitstream_converter_base();
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// ax25_bitstream_converter_adapter                                 //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct ax25_bitstream_converter_adapter : public bitstream_converter_base
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const override;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags = 45, int postamble_flags = 5) const override;
    bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read) override;
    bool try_decode(uint8_t bit, packet& p) override;
    void reset() override;

private:
    ax25_bitstream_converter converter;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// ax25_scrambled_bitstream_converter_adapter                        //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct ax25_scrambled_bitstream_converter_adapter : public bitstream_converter_base
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const override;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags = 45, int postamble_flags = 5) const override;
    bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read) override;
    bool try_decode(uint8_t bit, packet& p) override;
    void reset() override;

private:
    ax25_scrambled_bitstream_converter converter;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// fx25_bitstream_converter_adapter                                 //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct fx25_bitstream_converter_adapter : public bitstream_converter_base
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const override;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags = 45, int postamble_flags = 5) const override;
    bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read) override;
    bool try_decode(uint8_t bit, packet& p) override;
    void reset() override;

private:
    fx25_bitstream_converter converter;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// fx25_scrambled_bitstream_converter_adapter                        //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct fx25_scrambled_bitstream_converter_adapter : public bitstream_converter_base
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const override;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags = 45, int postamble_flags = 5) const override;
    bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read) override;
    bool try_decode(uint8_t bit, packet& p) override;
    void reset() override;

private:
    fx25_scrambled_bitstream_converter converter;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// il2p_bitstream_converter_adapter                                 //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct il2p_bitstream_converter_adapter : public bitstream_converter_base
{
    std::vector<uint8_t> encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const override;
    std::vector<uint8_t> encode(const packet& p, int preamble_flags = 0, int postamble_flags = 0) const override;
    bool try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read) override;
    bool try_decode(uint8_t bit, packet& p) override;
    void reset() override;

private:
    il2p_bitstream_converter converter;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// trim                                                             //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::string_view trim(std::string_view str);

// **************************************************************** //
//                                                                  //
//                                                                  //
//                                                                  //
// bitstream routines                                               //
//                                                                  //
// bytes_to_bits, bits_to_bytes, compute_crc                        //
// bit_stuff, nrzi_encode, add_hdlc_flags                           //
// encode_bitstream                                                 //
//                                                                  //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_AX25_NAMESPACE_BEGIN

template<typename InputIt, typename OutputIt>
OutputIt bytes_to_bits(InputIt first, InputIt last, OutputIt out);

template<typename InputIt, typename OutputIt>
OutputIt bits_to_bytes(InputIt first, InputIt last, OutputIt out);

template<typename InputIt>
std::array<uint8_t, 2> compute_crc(InputIt first, InputIt last);

template<typename InputIt>
LIBMODEM_INLINE std::array<uint8_t, 2> compute_crc_using_lut(InputIt first, InputIt last);

template<typename InputIt, typename OutputIt>
OutputIt bit_stuff(InputIt first, InputIt last, OutputIt out);

template<typename InputIt, typename OutputIt>
OutputIt bit_unstuff(InputIt first, InputIt last, OutputIt out);

template<typename InputIt, typename OutputIt>
OutputIt bit_unstuff_to_bytes(InputIt first, InputIt last, OutputIt out);

bool bit_unstuff_to_byte(uint8_t bit, uint8_t& byte, int& bit_index, int& stuff_count);

template<typename ForwardIt>
void nrzi_encode(ForwardIt first, ForwardIt last, uint8_t initial_level = 0);

template<typename InputIt, typename OutputIt>
OutputIt nrzi_encode(InputIt first, InputIt last, OutputIt out);

template<typename ForwardIt>
uint8_t nrzi_decode(ForwardIt first, ForwardIt last, uint8_t initial_value = 0);

uint8_t nrzi_decode(uint8_t bit, uint8_t last_nrzi_level);

uint8_t scramble_bit(uint8_t bit, uint32_t& state);

template<typename ForwardIt>
void scramble_bits(ForwardIt first, ForwardIt last);

template<typename OutputIt>
OutputIt add_hdlc_flags(OutputIt out, int count);

template<typename OutputIt>
OutputIt add_nrzi_hdlc_flags(OutputIt out, int count, uint8_t nrzi_level);

template<typename RandomIt>
RandomIt find_last_consecutive_hdlc_flag(RandomIt first, RandomIt last);

template<typename ForwardIt>
ForwardIt find_first_hdlc_flag(ForwardIt first, ForwardIt last);

bool ends_with_hdlc_flag(const std::vector<uint8_t>& bitstream);

template<typename InputIt>
bool ends_with_hdlc_flag(InputIt first, InputIt last);

bool hdlc_shift_register_detect(uint8_t decoded_bit, uint8_t& shift_register);

template<typename InputIt>
LIBMODEM_INLINE bool ends_with_hdlc_flag(InputIt first, InputIt last)
{
    static constexpr std::array<uint8_t, 8> hdlc_flag = { 0, 1, 1, 1, 1, 1, 1, 0 };

    if constexpr (std::random_access_iterator<InputIt>)
    {
        if (last - first < 8)
        {
            return false;
        }
        return std::equal(hdlc_flag.begin(), hdlc_flag.end(), last - 8);
    }
    else
    {
        auto count = std::distance(first, last);

        if (count < 8)
        {
            return false;
        }

        std::advance(first, count - 8);

        return std::equal(hdlc_flag.begin(), hdlc_flag.end(), first);
    }
}

template<typename InputIt, typename OutputIt>
LIBMODEM_INLINE OutputIt bytes_to_bits(InputIt first, InputIt last, OutputIt out)
{
    // Converts bytes to individual bits (LSB-first per byte)
    // 
    // Example: byte 0x7E (01111110) -> bits [0,1,1,1,1,1,1,0]

    for (auto it = first; it != last; ++it)
    {
        uint8_t byte = *it;
        for (int i = 0; i < 8; ++i)
        {
            *out++ = (byte >> i) & 1;  // Extract bits LSB-first
        }
    }

    return out;
}

template<typename InputIt, typename OutputIt>
LIBMODEM_INLINE OutputIt bytes_to_bits_stuffed(InputIt first, InputIt last, OutputIt out)
{
    // Converts bytes to individual bits (LSB-first per byte) with bit stuffing
    // Inserts a 0-bit after five consecutive 1-bits to prevent false flag detection
    //
    // Example: byte 0xFF (11111111) -> bits [1,1,1,1,1,0,1,1,1] (0 stuffed after 5th 1)
    //
    // Combines bytes_to_bits and bit_stuff into a single pass for efficiency
    // with no intermediate storage requirements, and such that it can work with an output iterator

    int count = 0;

    for (auto it = first; it != last; ++it)
    {
        uint8_t byte = *it;
        for (int i = 0; i < 8; ++i)
        {
            uint8_t bit = (byte >> i) & 1;  // Extract bits LSB-first
            *out++ = bit;

            if (bit == 1)
            {
                count += 1;
                if (count == 5)
                {
                    *out++ = 0;  // Stuff a zero
                    count = 0;
                }
            }
            else
            {
                count = 0;
            }
        }
    }

    return out;
}

template<typename InputIt, typename OutputIt>
LIBMODEM_INLINE OutputIt bits_to_bytes(InputIt first, InputIt last, OutputIt out)
{
    // Converts individual bits back to bytes (LSB-first per byte)
    // 
    // Example: bits [0,1,1,1,1,1,1,0] -> byte 0x7E (01111110)

    auto it = first;
    while (it != last)
    {
        uint8_t byte = 0;
        for (int i = 0; i < 8 && it != last; ++i)
        {
            if (*it++)
            {
                byte |= (1 << i);  // Set bit i if input bit is 1
            }
        }
        *out++ = byte;
    }

    return out;
}

template<typename InputIt>
LIBMODEM_INLINE std::array<uint8_t, 2> compute_crc(InputIt first, InputIt last)
{
    // Computes CRC-16-CCITT checksum for error detection in AX.25 frames
    // Uses reversed polynomial 0x8408 and processes bits LSB-first
    // 
    // Returns 2-byte CRC in little-endian format [low_byte, high_byte]

    const uint16_t poly = 0x8408; // CRC-16-CCITT reversed polynomial

    uint16_t crc = 0xFFFF;

    for (auto it = first; it != last; ++it)
    {
        uint8_t byte = *it;
        for (int i = 0; i < 8; ++i)
        {
            uint8_t bit = (byte >> i) & 1;  // LSB-first
            uint8_t xor_in = (crc ^ bit) & 0x01;
            crc >>= 1;
            if (xor_in)
            {
                crc ^= poly;
            }
        }
    }

    crc ^= 0xFFFF;
    return { static_cast<uint8_t>(crc & 0xFF), static_cast<uint8_t>((crc >> 8) & 0xFF) };
}

template<typename InputIt>
LIBMODEM_INLINE std::array<uint8_t, 2> compute_crc_using_lut(InputIt first, InputIt last)
{
    // Hardcoded CRC-16-CCITT lookup table for polynomial 0x8408 (reversed)
    // 
    // Table generation algorithm:
    // 
    // for (int i = 0; i < 256; ++i)
    // {
    //     uint16_t crc = i;
    //     for (int j = 0; j < 8; ++j)
    //     {
    //         if (crc & 1)
    //         {
    //             crc = (crc >> 1) ^ 0x8408;
    //         }
    //         else
    //         {
    //             crc >>= 1;
    //         }
    //     }
    //     table[i] = crc;
    // }
    //
    // Each entry represents the CRC remainder when dividing that byte value
    // by the polynomial, processing bits LSB-first

    static constexpr uint16_t crc_table[256] =
    {
        0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
        0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
        0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
        0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
        0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
        0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
        0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
        0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
        0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
        0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
        0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
        0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
        0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
        0xEF4E, 0xFEC7, 0xCC5C, 0xDDD5, 0xA96A, 0xB8E3, 0x8A78, 0x9BF1,
        0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
        0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
        0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
        0x0840, 0x19C9, 0x2B52, 0x3ADB, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
        0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
        0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
        0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
        0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
        0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
        0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
        0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
        0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
        0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
        0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
        0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
        0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
        0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
        0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78
    };

    uint16_t crc = 0xFFFF;
    for (auto it = first; it != last; ++it)
    {
        uint8_t table_index = (crc ^ *it) & 0xFF;
        crc = (crc >> 8) ^ crc_table[table_index];
    }

    crc ^= 0xFFFF;
    return { static_cast<uint8_t>(crc & 0xFF), static_cast<uint8_t>((crc >> 8) & 0xFF) };
}

uint16_t compute_crc_using_lut_init();
uint16_t compute_crc_using_lut_update(uint8_t byte, uint16_t crc);
std::array<uint8_t, 2> compute_crc_using_lut_finalize(uint16_t crc);

template<typename InputIt, typename OutputIt>
LIBMODEM_INLINE OutputIt bit_stuff(InputIt first, InputIt last, OutputIt out)
{
    // Inserts a 0-bit after five consecutive 1-bits to prevent false flag detection
    // Prevents data from accidentally looking like the HDLC flag byte (0x7E = 01111110)
    // 
    // Example:
    // 
    //   Input:  1 1 1 1 1 1 0
    //           ~~~~~~~~~
    //   Output: 1 1 1 1 1 0 1 0  (0 stuffed after 5th and 6th 1)
    //                     ~

    int count = 0;

    for (auto it = first; it != last; ++it)
    {
        *out++ = *it;  // Output the bit

        if (*it == 1)
        {
            count += 1;
            if (count == 5)
            {
                *out++ = 0;  // Stuff a zero
                count = 0;
            }
        }
        else
        {
            count = 0;
        }
    }

    return out;
}

template<typename InputIt, typename OutputIt>
LIBMODEM_INLINE OutputIt bit_unstuff(InputIt first, InputIt last, OutputIt out)
{
    // Removes stuffed 0-bits that were inserted after five consecutive 1-bits
    // This is the inverse operation of bit_stuff
    // 
    // Example:
    // 
    //   Input:  1 1 1 1 1 0 1 0  (0 stuffed after 5th 1)
    //   Output: 1 1 1 1 1 1 0

    int count = 0;

    for (auto it = first; it != last; ++it)
    {
        if (*it == 1)
        {
            *out++ = *it;
            count += 1;
        }
        else  // *it == 0
        {
            if (count == 5)
            {
                // This is a stuffed bit, skip it
                count = 0;
            }
            else
            {
                // This is a real data bit
                *out++ = *it;
                count = 0;
            }
        }
    }

    return out;
}

template<typename InputIt, typename OutputIt>
LIBMODEM_INLINE OutputIt bit_unstuff_to_bytes(InputIt first, InputIt last, OutputIt out)
{
    // Fused bit-unstuffing and bits-to-bytes conversion in a single pass.
    // Removes stuffed 0-bits after five consecutive 1-bits, and packs
    // the remaining bits into bytes LSB-first.

    uint8_t byte = 0;

    int stuff_count = 0;
    int bit_index = 0;

    for (auto it = first; it != last; ++it)
    {
        uint8_t bit = *it;

        if (bit == 1)
        {
            stuff_count++;
            byte |= (1 << bit_index);
            bit_index++;
        }
        else // bit == 0
        {
            if (stuff_count == 5)
            {
                // Stuffed bit, skip it
                stuff_count = 0;
                continue;
            }

            // Real zero bit
            bit_index++;
            stuff_count = 0;
        }

        if (bit_index == 8)
        {
            *out++ = byte;

            byte = 0;
            bit_index = 0;
        }
    }

    if (bit_index > 0)
    {
        *out++ = byte;
    }

    return out;
}

template<typename ForwardIt>
LIBMODEM_INLINE void nrzi_encode(ForwardIt first, ForwardIt last, uint8_t initial_level)
{
    // Encodes bitstream in-place to ensure signal transitions for clock recovery
    // NRZI: 0-bit = toggle level, 1-bit = keep level
    // 
    // Example:
    // 
    //   Input:  1 0 1 1 0 0 1
    //   Output: 0 1 1 1 0 1 1

    uint8_t level = initial_level; // Start at level 0 by default

    for (auto it = first; it != last; ++it)
    {
        if (*it == 0)
        {
            level ^= 1;
        }
        *it = level;
    }
}

template<typename ForwardIt>
LIBMODEM_INLINE uint8_t nrzi_decode(ForwardIt first, ForwardIt last, uint8_t initial_value)
{
    if (first == last)
    {
        return initial_value;
    }

    uint8_t prev = *first;
    uint8_t curr = 0;

    *first = initial_value;  // First bit ambiguous, often set to initial_value

    ++first;

    for (auto it = first; it != last; ++it)
    {
        curr = *it;
        *it = (curr == prev) ? 1 : 0;  // No transition=1, transition=0
        prev = curr;
    }

    return curr; // Return last level for chaining
}

template<typename ForwardIt>
LIBMODEM_INLINE void scramble_bits(ForwardIt first, ForwardIt last)
{
    uint32_t state = 0;

    for (auto it = first; it != last; ++it)
    {
        *it = scramble_bit(*it, state);
    }
}

template<typename OutputIt>
LIBMODEM_INLINE OutputIt add_hdlc_flags(OutputIt out, int count)
{
    constexpr uint8_t hdlc_flag = 0x7E;  // 01111110

    for (int j = 0; j < count; ++j)
    {
        for (int i = 0; i < 8; ++i)
        {
            *out++ = (hdlc_flag >> i) & 1;
        }
    }

    return out;
}

template<typename OutputIt>
LIBMODEM_INLINE OutputIt add_nrzi_hdlc_flags(OutputIt out, int count, uint8_t nrzi_level)
{
    // Generates NRZI-encoded HDLC flags. Each NRZ flag is 01111110.
    // NRZI encoding: 0 = transition (flip level), 1 = no transition (keep level).

    constexpr uint8_t hdlc_flag_nrz[8] = { 0, 1, 1, 1, 1, 1, 1, 0 };

    for (int j = 0; j < count; ++j)
    {
        for (int i = 0; i < 8; ++i)
        {
            if (hdlc_flag_nrz[i] == 0)
            {
                nrzi_level = nrzi_level ? 0 : 1;
            }
            *out++ = nrzi_level;
        }
    }

    return out;
}

template<typename RandomIt>
LIBMODEM_INLINE RandomIt find_last_consecutive_hdlc_flag(RandomIt first, RandomIt last)
{
    // Finds the last flag in a sequence of consecutive HDLC flags
    // Returns iterator to the start of the last flag, or last if not found

    constexpr std::array<uint8_t, 8> flag_pattern = { 0, 1, 1, 1, 1, 1, 1, 0 };

    auto current_preamble_flag = std::search(first, last, flag_pattern.begin(), flag_pattern.end());

    if (current_preamble_flag == last)
    {
        return last;
    }

    auto last_preamble_flag = current_preamble_flag;

    while (true)
    {
        auto next_search_start = last_preamble_flag + 8;

        if (next_search_start >= last)
        {
            break;
        }

        auto next_flag = std::search(next_search_start, last, flag_pattern.begin(), flag_pattern.end());

        if (next_flag == next_search_start)
        {
            last_preamble_flag = next_flag;
        }
        else
        {
            // Found a gap or no more flags - frame data starts here
            break;
        }
    }

    return last_preamble_flag;
}

template<typename ForwardIt>
LIBMODEM_INLINE ForwardIt find_first_hdlc_flag(ForwardIt first, ForwardIt last)
{
    // Finds the first HDLC flag in the bitstream
    // Returns iterator to the start of the flag, or last if not found

    constexpr std::array<uint8_t, 8> flag_pattern = { 0, 1, 1, 1, 1, 1, 1, 0 };

    return std::search(first, last, flag_pattern.begin(), flag_pattern.end());
}

LIBMODEM_AX25_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// AX.25                                                            //
//                                                                  //
// encode_header, encode_addresses, encode_address, encode_frame    //
// try_decode_frame, try_decode_packet, encode_bitstream            //
// try_parse_address, parse_addresses, try_decode_bitstream         //
//                                                                  //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_AX25_NAMESPACE_BEGIN

struct nrz_scrambled_t
{
};

inline constexpr nrz_scrambled_t nrz_scrambled;

struct from_nrzi_bits_t
{
};

inline constexpr from_nrzi_bits_t from_nrzi_bits;

struct nrzi_bits_t
{
};

inline constexpr nrzi_bits_t nrzi_bits;

template <typename InputIt, typename OutputIt>
std::pair<OutputIt, bool> try_parse_address(InputIt first, InputIt last, OutputIt out, int& ssid, bool& cr_or_h_bit);

template <typename InputIt, typename OutputIt>
std::pair<OutputIt, bool> try_parse_address(InputIt first_it, InputIt last_it, OutputIt out, int& ssid, bool& cr_or_h_bit, bool& last);

template <typename InputIt, typename OutputIt>
std::pair<OutputIt, bool> try_parse_address(InputIt first_it, InputIt last_it, OutputIt out, int& ssid, bool& cr_or_h_bit, bool& last, std::array<uint8_t, 2>& reserved_bits);

template <typename InputIt>
bool try_parse_address(InputIt first, InputIt last, std::string& address_text, int& ssid, bool& cr_or_h_bit);

template <typename InputIt>
bool try_parse_address(InputIt first, InputIt last, std::string& address_text, int& ssid, bool& cr_or_h_bit, std::array<uint8_t, 2>& reserved_bits);

template <typename InputIt>
bool try_parse_address(InputIt first, InputIt last, struct address& address);

template <typename InputIt, typename OutputIt>
OutputIt parse_addresses(InputIt first, InputIt last, OutputIt out);

bool try_parse_address(std::string_view data, std::string& address, int& ssid, bool& cr_or_h_bit);
bool try_parse_address(std::string_view data, struct address& address);

void parse_addresses(std::string_view data, std::vector<address>& addresses);

std::vector<uint8_t> encode_frame(const packet& p);
std::vector<uint8_t> encode_frame(const struct frame& frame);

template <typename InputIt>
std::vector<uint8_t> encode_frame(const address& from, const address& to, const std::vector<address>& path, InputIt input_it_first, InputIt input_it_last);

template <typename Container, typename Traits>
Container encode_frame(const address& from, const address& to, const std::vector<address>& path, uint8_t control);

template <typename Container, typename Traits>
Container encode_frame_without_crc(const address& from, const address& to, const std::vector<address>& path, uint8_t control);

template <typename InputIt>
std::vector<uint8_t> encode_frame(const address& from, const address& to, const std::vector<address>& path, InputIt input_it_first, InputIt input_it_last, uint8_t control, uint8_t pid);

template <typename Container, typename InputIt, typename Traits = container_traits<Container>>
Container encode_frame(const address& from, const address& to, const std::vector<address>& path, InputIt data_it_first, InputIt data_it_last);

template <typename Container, typename InputIt, typename Traits = container_traits<Container>>
Container encode_frame(const address& from, const address& to, const std::vector<address>& path, InputIt data_it_first, InputIt data_it_last, uint8_t control, uint8_t pid);

template <typename ForwardIt1, typename InputIt, typename ForwardIt2>
ForwardIt2 encode_frame(const address& from, const address& to, ForwardIt1 path_first_it, ForwardIt1 path_last_it, InputIt data_it_first, InputIt data_it_last, ForwardIt2 out);

template <typename ForwardIt1, typename ForwardIt2>
ForwardIt2 encode_frame(const address& from, const address& to, ForwardIt1 path_first_it, ForwardIt1 path_last_it, uint8_t control, ForwardIt2 out);

template <typename ForwardIt, typename OutputIt>
OutputIt encode_frame_without_crc(const address& from, const address& to, ForwardIt path_first_it, ForwardIt path_last_it, uint8_t control, OutputIt out);

template <typename ForwardIt1, typename InputIt, typename ForwardIt2>
ForwardIt2 encode_frame(const address& from, const address& to, ForwardIt1 path_first_it, ForwardIt1 path_last_it, InputIt data_it_first, InputIt data_it_last, uint8_t control, uint8_t pid, ForwardIt2 out);

template <typename ForwardIt>
ForwardIt encode_frame(const packet& p, ForwardIt out);

bool try_decode_frame(const std::vector<uint8_t>& frame_bytes, packet& p);
bool try_decode_frame(const std::vector<uint8_t>& frame_bytes, struct frame& frame);
bool try_decode_frame(const std::vector<uint8_t>& frame_bytes, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data);
bool try_decode_frame(const std::vector<uint8_t>& frame_bytes, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data, std::array<uint8_t, 2>& crc);
bool try_decode_frame_no_fcs(const std::vector<uint8_t>& frame_bytes, packet& p);
bool try_decode_frame_no_fcs(const std::vector<uint8_t>& frame_bytes, struct frame& frame);
bool try_decode_frame_no_fcs(const std::vector<uint8_t>& frame_bytes, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data);
bool try_decode_frame_no_fcs(std::span<const uint8_t> frame_bytes, packet& p);
bool try_decode_frame_no_fcs(std::span<const uint8_t> frame_bytes, struct frame& frame);
bool try_decode_frame_no_fcs(std::span<const uint8_t> frame_bytes, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data);

template<class InputIt>
bool try_decode_packet(InputIt frame_it_first, InputIt frame_it_last, packet& p);

template<class InputIt>
bool try_decode_frame(InputIt frame_it_first, InputIt frame_it_last, struct frame& frame);

template<typename RandomIt>
bool try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data, std::array<uint8_t, 2>& crc);

template<typename RandomIt>
bool try_decode_frame_no_fcs(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data);

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, std::array<uint8_t, 2>& crc);

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame_no_fcs(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data);

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& crc);

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename InputIt, typename ForwardIt, typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(from_nrzi_bits_t, InputIt frame_it_first, InputIt frame_it_last, ForwardIt unstuffed_bits_it, RandomIt frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename InputIt, typename ForwardIt, typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(from_nrzi_bits_t, InputIt frame_it_first, InputIt frame_it_last, ForwardIt unstuffed_bits_it, RandomIt frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename InputIt, typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<RandomIt, OutputIt1, OutputIt2, bool> try_decode_frame(from_nrzi_bits_t, InputIt frame_it_first, InputIt frame_it_last, RandomIt frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename InputIt, typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<RandomIt, OutputIt1, OutputIt2, bool> try_decode_frame(from_nrzi_bits_t, InputIt frame_it_first, InputIt frame_it_last, RandomIt frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame_no_fcs(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid);

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame_no_fcs(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid);

std::vector<uint8_t> encode_header(const packet& p);
std::vector<uint8_t> encode_header(const address& from, const address& to, const std::vector<address>& path);

template<typename OutputIt>
OutputIt encode_header(const address& from, const address& to, const std::vector<address>& path, OutputIt out);

template<typename ForwardIt, typename OutputIt>
OutputIt encode_header(const address& from, const address& to, ForwardIt path_first_it, ForwardIt path_last_it, OutputIt out);

std::vector<uint8_t> encode_addresses(const std::vector<address>& path);

template<typename OutputIt>
OutputIt encode_addresses(const std::vector<address>& path, OutputIt out);

template<typename ForwardIt, typename OutputIt>
OutputIt encode_addresses(ForwardIt path_first_it, ForwardIt path_last_it, OutputIt out);

std::array<uint8_t, 7> encode_address(const struct address& address, bool last);
std::array<uint8_t, 7> encode_address(std::string_view address, int ssid, bool cr_or_h_bit, bool last);
std::array<uint8_t, 7> encode_address(std::string_view address, int ssid, bool cr_or_h_bit, bool last, std::array<uint8_t, 2> reserved_bits);

std::vector<uint8_t> encode_bitstream(const packet& p, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(const packet& p, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(const frame& f, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(const frame& f, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(const std::vector<uint8_t>& frame, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(const std::vector<uint8_t>& frame, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);

std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const packet& p, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const packet& p, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const frame& f, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const frame& f, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const std::vector<uint8_t>& frame, int preamble_flags, int postamble_flags);
std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const std::vector<uint8_t>& frame, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);

template<typename InputIt>
std::vector<uint8_t> encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags);

template<typename InputIt>
std::vector<uint8_t> encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);

template<typename InputIt>
std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags);

template<typename InputIt>
std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);

template <typename Container, typename InputIt, typename Traits = container_traits<Container>>
Container encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags);

template <typename Container, typename InputIt, typename Traits = container_traits<Container>>
Container encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);

template <typename Container, typename InputIt, typename Traits = container_traits<Container>>
Container encode_bitstream(nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags);

template <typename Container, typename InputIt, typename Traits = container_traits<Container>>
Container encode_bitstream(nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags);

template<typename InputIt, typename ForwardIt>
ForwardIt encode_bitstream(InputIt frame_first, InputIt frame_last, int preamble_flags, int postamble_flags, ForwardIt out);

template<typename InputIt, typename ForwardIt>
ForwardIt encode_bitstream(InputIt frame_first, InputIt frame_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags, ForwardIt out);

template<typename InputIt, typename ForwardIt>
ForwardIt encode_bitstream(nrz_scrambled_t, InputIt frame_first, InputIt frame_last, int preamble_flags, int postamble_flags, ForwardIt out);

template<typename InputIt, typename ForwardIt>
ForwardIt encode_bitstream(nrz_scrambled_t, InputIt frame_first, InputIt frame_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags, ForwardIt out);

bool try_decode_bitstream(uint8_t bit, bitstream_state& state, std::vector<uint8_t>& bitstream_buffer);
bool try_decode_bitstream(uint8_t bit, bitstream_state& state, std::vector<uint8_t>& bitstream_buffer, struct frame& frame);
bool try_decode_bitstream(uint8_t bit, packet& packet, bitstream_state& state, std::vector<uint8_t>& bitstream_buffer);
bool try_decode_bitstream(const std::vector<uint8_t>& bitstream, size_t offset, packet& packet, size_t& read, bitstream_state& state, std::vector<uint8_t>& bitstream_buffer);

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
std::tuple<OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename RandomIt, size_t N, typename OutputIt1, typename OutputIt2>
std::tuple<size_t, OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, std::array<uint8_t, N>& frame_bytes, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename RandomIt, size_t N, typename OutputIt1, typename OutputIt2>
std::tuple<size_t, OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, std::array<uint8_t, N>& frame_bytes, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename RandomIt1, typename RandomIt2, typename OutputIt1, typename OutputIt2>
std::tuple<RandomIt2, OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt1 bitstream_it_first, RandomIt1 bitstream_it_last, RandomIt2 frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename RandomIt1, typename RandomIt2, typename OutputIt1, typename OutputIt2>
std::tuple<RandomIt2, OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt1 bitstream_it_first, RandomIt1 bitstream_it_last, RandomIt2 frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc);

template<typename RandomIt, typename OutputIt>
std::pair<OutputIt, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, OutputIt frame_bytes_it);

template <typename InputIt, typename OutputIt>
LIBMODEM_INLINE std::pair<OutputIt, bool> try_parse_address(InputIt first_it, InputIt last_it, OutputIt out, int& ssid, bool& cr_or_h_bit)
{
    bool last = false;
    return try_parse_address(first_it, last_it, out, ssid, cr_or_h_bit, last);
}

template <typename InputIt, typename OutputIt>
LIBMODEM_INLINE std::pair<OutputIt, bool> try_parse_address(InputIt first_it, InputIt last_it, OutputIt out, int& ssid, bool& cr_or_h_bit, bool& last)
{
    std::array<uint8_t, 2> reserved_bits = { 1, 1 };
    return try_parse_address(first_it, last_it, out, ssid, cr_or_h_bit, last, reserved_bits);
}

template <typename InputIt, typename OutputIt>
LIBMODEM_INLINE std::pair<OutputIt, bool> try_parse_address(InputIt first_it, InputIt last_it, OutputIt out, int& ssid, bool& cr_or_h_bit, bool& last, std::array<uint8_t, 2>& reserved_bits)
{
    // Parse an AX.25 address
    //
    // AX.25 addresses are always exactly 7 bytes:
    // 
    //  - Bytes 0-5: Callsign (6 characters, space-padded)
    //    - Each character is left-shifted by 1 bit
    //  - Byte 6
    //    - Bits 1-4: SSID
    //    - Bit 0: Last address marker
    //    - Bit 7: H-bit (used/marked), or C-bit (command/response) depending on context

    char address_text[7] = { '\0' }; // addresses are 6 characters long

    for (size_t i = 0; i < 6; i++)
    {
        if (first_it == last_it)
        {
            return { out, false }; // Fewer than 6 bytes
        }

        // data is organized in 7 bits
        // one bit is unused and reserved as future extension
        address_text[i] = static_cast<uint8_t>(*first_it++) >> 1;
    }

    if (first_it == last_it)
    {
        return { out, false }; // Missing byte 7
    }

    // The ssid is shifted left by 1 bit, bits 1-4 contain the SSID
    ssid = (static_cast<uint8_t>(*first_it) >> 1) & 0b00001111; // 0xF mask for bits 1-4

    cr_or_h_bit = (static_cast<uint8_t>(*first_it) & 0b10000000) != 0; // 0x80 mask for the C/H bit in the last byte

    last = (static_cast<uint8_t>(*first_it) & 0b00000001) != 0; // 0x01 mask for the last address marker

    // Bits 5-6 are reserved
    // The value of these bits can be ignored
    // We provide retrieval for testing purposes
    reserved_bits[0] = (static_cast<uint8_t>(*first_it) >> 5) & 0b00000001;
    reserved_bits[1] = (static_cast<uint8_t>(*first_it) >> 6) & 0b00000001;

    std::string_view address_text_trimmed = trim(address_text);

    out = std::copy(address_text_trimmed.begin(), address_text_trimmed.end(), out);

    return { out, true };
}

template <typename InputIt>
LIBMODEM_INLINE bool try_parse_address(InputIt first, InputIt last, std::string& address_text, int& ssid, bool& cr_or_h_bit)
{
    std::array<uint8_t, 2> reserved_bits = { 1, 1 };
    return try_parse_address(first, last, address_text, ssid, cr_or_h_bit, reserved_bits);
}

template <typename InputIt>
LIBMODEM_INLINE bool try_parse_address(InputIt first, InputIt last, std::string& address_text, int& ssid, bool& cr_or_h_bit, std::array<uint8_t, 2>& reserved_bits)
{
    bool last_address = false;
    auto [_, result] = try_parse_address(first, last, std::back_inserter(address_text), ssid, cr_or_h_bit, last_address, reserved_bits);
    return result;
}

template <typename InputIt>
LIBMODEM_INLINE bool try_parse_address(InputIt first, InputIt last, struct address& address)
{
    address.text = {};
    address.text_length = 0;
    address.ssid = 0;
    address.mark = false;
    address.command_response = false;
    address.reserved_bits = { 1, 1 };

    bool cr_or_h_bit = false;
    bool last_address = false;

    auto [out_it, result] = try_parse_address(first, last, address.text.data(), address.ssid, cr_or_h_bit, last_address, address.reserved_bits);
    if (result)
    {
        address.text_length = static_cast<size_t>(out_it - address.text.data());
        address.command_response = cr_or_h_bit;
        address.mark = cr_or_h_bit;
    }

    return result;
}

template <typename ForwardIt, typename OutputIt>
LIBMODEM_INLINE OutputIt parse_addresses(ForwardIt first, ForwardIt last, OutputIt out)
{
    while (std::distance(first, last) >= 7)
    {
        struct address address;
        auto next = std::next(first, 7);
        LIBMODEM_AX25_NAMESPACE_REFERENCE try_parse_address(first, next, address);
        *out++ = address;
        first = next;
    }

    return out;
}

template<typename ForwardIt1, typename ForwardIt2>
LIBMODEM_INLINE bool validate_frame(const address& from, const address& to, ForwardIt1 path_first, ForwardIt1 path_last, ForwardIt2 data_first, ForwardIt2 data_last, uint8_t control, uint8_t pid, const std::array<uint8_t, 2>& actual_crc, const std::array<uint8_t, 2>& expected_crc)
{
    if (!validate_address(from) || !validate_address(to))
    {
        return false;
    }

    size_t path_count = static_cast<size_t>(std::distance(path_first, path_last));
    if (path_count > 8)
    {
        return false;
    }

    for (auto it = path_first; it != path_last; ++it)
    {
        if (!validate_address(*it))
        {
            return false;
        }
    }

    frame_type type = get_frame_type(control);
    if (type == frame_type::unknown)
    {
        return false;
    }

    // I-frames and UI frames must have a valid PID
    bool is_i_or_ui_frame = (type == frame_type::i || type == frame_type::ui);
    if (is_i_or_ui_frame)
    {
        if (!validate_pid(pid))
        {
            return false;
        }

        // Validate data length is reasonable
        size_t data_length = static_cast<size_t>(std::distance(data_first, data_last));
        if (data_length > 256)
        {
            return false;
        }
    }

    // S-frames carry no PID and no data
    if (is_s_frame_type(type))
    {
        size_t data_length = static_cast<size_t>(std::distance(data_first, data_last));
        if (data_length > 0)
        {
            return false;
        }
    }

    if (actual_crc != expected_crc)
    {
        return false;
    }

    return true;
}

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    constexpr size_t max_data_length = SIZE_MAX;
    return try_decode_bitstream(bit, state, bitstream_it_first, bitstream_it_last, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
}

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    // Raw bits accumulate in the bitstream buffer (bitstream_it) across calls;
    // frame_bytes is only written once when a complete frame is converted from bits to bytes
    std::array<uint8_t, 1000> frame_bytes = {};
    auto [frame_bytes_size, path_out, data_out, result] = try_decode_bitstream(bit, state, bitstream_it_first, bitstream_it_last, frame_bytes, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
    return { path_out, data_out, result };
}

template<typename RandomIt, size_t N, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<size_t, OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, std::array<uint8_t, N>& frame_bytes, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    constexpr size_t max_data_length = SIZE_MAX;
    return try_decode_bitstream(bit, state, bitstream_it_first, bitstream_it_last, frame_bytes, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
}

template<typename RandomIt, size_t N, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<size_t, OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, std::array<uint8_t, N>& frame_bytes, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    auto [frame_bytes_end, path_out, data_out, result] = try_decode_bitstream(bit, state, bitstream_it_first, bitstream_it_last, frame_bytes.begin(), from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
    size_t frame_bytes_size = static_cast<size_t>(std::distance(frame_bytes.begin(), frame_bytes_end));
    return { frame_bytes_size, path_out, data_out, result };
}

template<typename RandomIt1, typename RandomIt2, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<RandomIt2, OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt1 bitstream_it_first, RandomIt1 bitstream_it_last, RandomIt2 frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    constexpr size_t max_data_length = SIZE_MAX;
    return try_decode_bitstream(bit, state, bitstream_it_first, bitstream_it_last, frame_bytes_it, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
}

template<typename RandomIt1, typename RandomIt2, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<RandomIt2, OutputIt1, OutputIt2, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt1 bitstream_it_first, RandomIt1 bitstream_it_last, RandomIt2 frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    auto [frame_bytes_end, result] = try_decode_bitstream_bare(bit, state, bitstream_it_first, bitstream_it_last, frame_bytes_it);

    if (result)
    {
        auto [path_out, data_out, decode_result] = try_decode_frame(frame_bytes_it, frame_bytes_end, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
        return { frame_bytes_end, path_out, data_out, decode_result };
    }

    return { frame_bytes_it, path, data, false };
}

template<typename RandomIt, typename OutputIt>
LIBMODEM_INLINE std::pair<OutputIt, bool> try_decode_bitstream(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, OutputIt frame_bytes_it)
{
    // Process one bit at a time through the AX.25 bitstream decoding pipeline:
    //
    // 1. NRZI decode the incoming raw bit
    // 2. Add decoded bit to external bitstream buffer
    // 3. Check for HDLC flag patterns (0x7E = 01111110)
    // 4. State machine:
    //    - searching: Looking for first preamble flag
    //    - in_preamble: Found flag(s), waiting for frame data or more flags
    //    - in_frame: Collecting frame data until postamble flag
    // 5. When postamble found, decode the accumulated frame
    //
    // Returns true when a complete frame has been decoded

    // After a successful decode, we preserve the state (in_preamble with the shared flag)
    // Only reset the complete flag, not the entire state - this allows shared flags to work
    // where the postamble of one packet serves as the preamble of the next
    if (state.complete)
    {
        // Deferred buffer shift
        // The raw bitstream was preserved for the caller to read
        // Now shift the postamble flag to the start for the next frame
        std::copy(bitstream_it_first + state.frame_end_index, bitstream_it_first + state.bitstream_size, bitstream_it_first);
        state.bitstream_size = state.bitstream_size - state.frame_end_index;
        state.frame_start_index = state.bitstream_size;
        state.complete = false;
    }

    // NRZI decode: no transition = 1, transition = 0
    uint8_t decoded_bit = nrzi_decode(bit, state.last_nrzi_level);

    state.last_nrzi_level = bit;

    size_t capacity = static_cast<size_t>(std::distance(bitstream_it_first, bitstream_it_last));

    // Add decoded bit to buffer
    if (state.bitstream_size < capacity)
    {
        *(bitstream_it_first + state.bitstream_size) = decoded_bit;
        state.bitstream_size++;
    }
    else
    {
        // Buffer full, reset to search mode
        state.searching = true;
        state.in_frame = false;
        state.in_preamble = false;
        state.bitstream_size = 0;
        state.frame_start_index = 0;
        state.global_preamble_start_pending = 0;
        state.frame_nrzi_level_pending = 0;
        state.preamble_count_pending = 0;
        state.postamble_count_pending = 0;
        return { frame_bytes_it, false };
    }

    state.global_bit_count++;

    // Check for HDLC flag pattern in the last 8 bits
    bool found_hdlc_flag = ends_with_hdlc_flag(bitstream_it_first, bitstream_it_first + state.bitstream_size);

    if (state.searching)
    {
        // Looking for the first HDLC flag
        if (found_hdlc_flag)
        {
            state.searching = false;
            state.in_preamble = true;
            state.frame_start_index = state.bitstream_size; // Frame starts after this flag
            state.preamble_count_pending = 1;
            state.postamble_count_pending = 0;

            if (state.enable_diagnostics)
            {
                // Track where preamble started (first bit of this flag)
                state.global_preamble_start_pending = state.global_bit_count - 7;

                // Compute NRZI level before the preamble by working backwards through the 8 flag bits
                uint8_t level = state.last_nrzi_level;
                for (size_t i = 0; i < 8 && i < state.bitstream_size; i++)
                {
                    if (*(bitstream_it_first + state.bitstream_size - 1 - i) == 0)
                    {
                        level = level ? 0 : 1;
                    }
                }
                state.frame_nrzi_level_pending = level;
            }
        }
        else if (state.bitstream_size > 16)
        {
            // Optimization: prevent buffer from growing indefinitely while searching
            // Keep only the last 8 bits needed for flag detection
            std::copy(bitstream_it_first + state.bitstream_size - 8, bitstream_it_first + state.bitstream_size, bitstream_it_first);
            state.bitstream_size = 8;
        }
    }
    else if (state.in_preamble)
    {
        // We've seen at least one flag. Check if this is another consecutive flag
        // or if frame data has started.
        if (found_hdlc_flag)
        {
            // Another consecutive flag, update frame start position
            state.frame_start_index = state.bitstream_size;
            state.preamble_count_pending++;
        }
        else
        {
            // Check if we have at least 8 bits since the last flag
            // to confirm we're in frame data (not still at a flag boundary)
            if (state.bitstream_size >= state.frame_start_index + 8)
            {
                state.in_preamble = false;
                state.in_frame = true;
            }
        }
    }
    else if (state.in_frame)
    {
        // Collecting frame data. Check for postamble flag.
        if (found_hdlc_flag)
        {
            state.postamble_count_pending = 1;

            // Found postamble! Extract frame bits (excluding the 8-bit postamble flag)
            size_t frame_end = state.bitstream_size - 8;

            if (frame_end > state.frame_start_index)
            {
                auto frame_begin_it = bitstream_it_first + state.frame_start_index;
                auto frame_end_it = bitstream_it_first + frame_end;

                size_t frame_bits_size = frame_end - state.frame_start_index;

                // Convert frame bits to bytes in caller-provided buffer
                auto frame_bytes_end = bit_unstuff_to_bytes(frame_begin_it, frame_end_it, frame_bytes_it);

                // Set the global bit positions for the successfully found frame
                state.global_preamble_start = state.global_preamble_start_pending;
                state.global_postamble_end = state.global_bit_count;
                state.frame_nrzi_level = state.frame_nrzi_level_pending;

                // Save frame boundaries for caller access
                // The raw bitstream bits at [frame_start_index, frame_end_index) remain valid until the next call
                // The buffer shift is deferred to the next call when state.complete is cleared
                state.frame_end_index = frame_end;
                state.in_preamble = true; // Ready for next frame
                state.in_frame = false;
                // If we found a valid frame, set complete flag regardless of whether the packet was decoded successfully
                state.complete = true;
                state.preamble_count = state.preamble_count_pending;
                state.postamble_count = state.postamble_count_pending;
                state.preamble_count_pending = 1;
                state.postamble_count_pending = 0;

                state.frame_size_bits = frame_bits_size;

                if (state.enable_diagnostics)
                {
                    // Set up tracking for potential next frame using the shared flag
                    state.global_preamble_start_pending = state.global_bit_count - 7;

                    // Compute NRZI level before this shared flag
                    uint8_t level = state.last_nrzi_level;
                    for (size_t i = 0; i < 8; i++)
                    {
                        if (*(bitstream_it_first + state.bitstream_size - 1 - i) == 0)
                        {
                            level = level ? 0 : 1;
                        }
                    }
                    state.frame_nrzi_level_pending = level;
                }

                return { frame_bytes_end, true };
            }
            else
            {
                // Empty frame - just consecutive flags, stay in preamble mode
                state.frame_start_index = state.bitstream_size;
                state.in_frame = false;
                state.in_preamble = true;
            }
        }

        // Continue collecting frame bits
        //
        // Safety check: prevent runaway buffer growth (max reasonable AX.25 frame)
        // AX.25 max frame is ~330 bytes = 2640 bits, with bit stuffing could be ~3200 bits
        // Add preamble flags overhead, let's say 4000 bits max

        if (state.bitstream_size > (state.max_frame_bits > 0 ? state.max_frame_bits : 8000))
        {
            // Something went wrong (noise, lost sync), reset to search mode
            state.searching = true;
            state.in_frame = false;
            state.bitstream_size = 0;
            state.frame_start_index = 0;
            state.global_preamble_start_pending = 0;
            state.frame_nrzi_level_pending = 0;
            state.preamble_count_pending = 0;
            state.postamble_count_pending = 0;
        }
    }

    return { frame_bytes_it, false }; // No complete frame yet
}

template<typename BitFormat = void, typename RandomIt>
LIBMODEM_INLINE std::pair<RandomIt, bool> try_decode_bitstream_bare(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last)
{
    // Raw bits decoder. Stores decoded (NRZ) or raw (NRZI) bits into [bitstream_it_first, bitstream_it_last)
    // depending on BitFormat. Uses shift register for O(1) HDLC flag detection.
    // Returns iterator past the last valid frame bit and true on frame completion.
    // Frame bits at [bitstream_it_first, returned_iterator) exclude preamble and postamble flags.

    if (state.complete)
    {
        state.bitstream_size = 0;
        state.complete = false;
    }

    // NRZI decode: no transition = 1, transition = 0
    uint8_t decoded_bit = nrzi_decode(bit, state.last_nrzi_level);
    state.last_nrzi_level = bit;
    state.global_bit_count++;

    uint8_t stored_bit;

    // Select which bit to store based on BitFormat
    if constexpr (std::is_same_v<BitFormat, nrzi_bits_t>)
    {
        stored_bit = bit;
    }
    else
    {
        stored_bit = decoded_bit;
    }

    // Check for HDLC flag via shift register
    bool found_hdlc_flag = hdlc_shift_register_detect(decoded_bit, state.hdlc_shift_register) && (state.global_bit_count >= 8);

    size_t capacity = static_cast<size_t>(std::distance(bitstream_it_first, bitstream_it_last));

    // Store bit when actively tracking preamble or frame
    if (!state.searching)
    {
        if (state.bitstream_size < capacity)
        {
            *(bitstream_it_first + state.bitstream_size) = stored_bit;
            state.bitstream_size++;
        }
        else
        {
            // Buffer overflow: reset to search mode
            state.searching = true;
            state.in_frame = false;
            state.in_preamble = false;
            state.bitstream_size = 0;
            state.frame_start_index = 0;
            state.preamble_count_pending = 0;
            state.postamble_count_pending = 0;
            state.frame_nrzi_level_pending = 0;
            state.hdlc_shift_register = 0;

            return { bitstream_it_first, false };
        }
    }

    if (state.searching)
    {
        if (found_hdlc_flag)
        {
            state.searching = false;
            state.in_preamble = true;
            state.bitstream_size = 0;
            state.preamble_count_pending = 1;
            state.postamble_count_pending = 0;
            state.frame_nrzi_level_pending = state.last_nrzi_level;
        }
    }
    else if (state.in_preamble)
    {
        if (found_hdlc_flag)
        {
            // Another consecutive flag; reset buffer to start fresh
            state.bitstream_size = 0;
            state.preamble_count_pending++;
        }
        else
        {
            // Wait for 8 non-flag bits to confirm frame data started
            if (state.bitstream_size >= 8)
            {
                state.in_preamble = false;
                state.in_frame = true;
                state.aborted = false;
            }
        }
    }
    else if (state.in_frame)
    {
        // Abort detection: seven or eight consecutive 1s indicate an abort sequence
        // Shift register layout: newest bit at LSB (left-shift)
        // 0x7F = 01111111 = oldest 0, seven newest are 1 (exactly 7 ones ending at current bit)
        // 0xFF = 11111111 = eight consecutive 1s
        if ((state.hdlc_shift_register & 0xFF) == 0x7F || (state.hdlc_shift_register & 0xFF) == 0xFF)
        {
            state.aborted = true;
        }

        if (found_hdlc_flag)
        {
            // postamble_count is a sentinel: exactly one closing flag is attributed to the
            // frame. The wire cannot distinguish additional trailing flags from the next
            // frame's preamble flags (e.g., 4 postamble + 50 preamble on the wire is just 54
            // consecutive flags with no boundary marker), so we commit to "first flag closes
            // the frame, all others belong to the next frame's preamble". For the last frame
            // in a stream, callers may read state.preamble_count_pending at end of stream to
            // recover the remaining trailing flag count unambiguously attributable to it.
            state.postamble_count_pending = 1;

            // Frame bits at [0, bitstream_size - 8); the last 8 bits are the postamble flag
            size_t frame_end = state.bitstream_size - 8;

            if (frame_end > 0)
            {
                state.frame_nrzi_level = state.frame_nrzi_level_pending;
                state.preamble_nrzi_level = state.frame_nrzi_level_pending;
                state.frame_nrzi_level_pending = state.last_nrzi_level;

                state.frame_start_index = 0;
                state.frame_end_index = frame_end;
                state.frame_size_bits = frame_end;

                state.in_preamble = true;
                state.in_frame = false;
                state.complete = true;
                state.preamble_count = state.preamble_count_pending;
                state.postamble_count = state.postamble_count_pending;

                // HDLC flag attribution at frame boundary: the flag that closes this
                // frame also serves as the opening flag of any subsequent frame. We
                // already counted it as part of this frame's postamble, so reset the
                // next frame's pending preamble count to 0 to avoid double-counting.
                // (Change to 1 to attribute the shared flag to the next frame's preamble instead.)
                state.preamble_count_pending = 0;

                state.postamble_count_pending = 0;
                state.bitstream_size = 0;

                return { bitstream_it_first + frame_end, true };
            }
            else
            {
                // Empty frame (consecutive flags)
                state.in_frame = false;
                state.in_preamble = true;
                state.bitstream_size = 0;
        }
                }

        // Prevent runaway buffer growth
        size_t effective_max = state.max_frame_bits > 0 ? state.max_frame_bits : capacity;
        if (state.bitstream_size > effective_max)
                {
                    state.searching = true;
                    state.in_frame = false;
            state.bitstream_size = 0;
            state.frame_start_index = 0;
            state.preamble_count_pending = 0;
            state.postamble_count_pending = 0;
            state.frame_nrzi_level_pending = 0;
                    state.hdlc_shift_register = 0;
                }
            }

    return { bitstream_it_first, false };
        }

template<typename RandomIt, typename OutputIt>
LIBMODEM_INLINE std::pair<OutputIt, bool> try_decode_bitstream_bare(uint8_t bit, bitstream_state& state, RandomIt bitstream_it_first, RandomIt bitstream_it_last, OutputIt frame_bytes_it)
{
    // Frame bytes wrapper around raw bits core. Stores decoded NRZ bits into the
    // bitstream buffer, then converts to frame bytes via bit_unstuff_to_bytes on completion.

    auto [bitstream_end, result] = try_decode_bitstream_bare(bit, state, bitstream_it_first, bitstream_it_last);

    if (result)
    {
        auto frame_bytes_end = bit_unstuff_to_bytes(bitstream_it_first, bitstream_end, frame_bytes_it);
        return { frame_bytes_end, true };
    }

    return { frame_bytes_it, false }; // No complete frame yet
}

template<typename RandomIt1, typename RandomIt2, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<RandomIt2, OutputIt1, OutputIt2, bool> try_decode_bitstream_bare(uint8_t bit, bitstream_state& state, RandomIt1 bitstream_it_first, RandomIt1 bitstream_it_last, RandomIt2 frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    constexpr size_t max_data_length = SIZE_MAX;
    return try_decode_bitstream_bare(bit, state, bitstream_it_first, bitstream_it_last, frame_bytes_it, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
}

template<typename RandomIt1, typename RandomIt2, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<RandomIt2, OutputIt1, OutputIt2, bool> try_decode_bitstream_bare(uint8_t bit, bitstream_state& state, RandomIt1 bitstream_it_first, RandomIt1 bitstream_it_last, RandomIt2 frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    auto [frame_bytes_end, result] = try_decode_bitstream_bare(bit, state, bitstream_it_first, bitstream_it_last, frame_bytes_it);

    if (result)
    {
        auto [path_out, data_out, decode_result] = try_decode_frame(frame_bytes_it, frame_bytes_end, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
        return { frame_bytes_end, path_out, data_out, decode_result };
    }

    return { frame_bytes_it, path, data, false };
}

template <typename InputIt>
LIBMODEM_INLINE std::vector<uint8_t> encode_frame(const address& from, const address& to, const std::vector<address>& path, InputIt data_it_first, InputIt data_it_last)
{
    return encode_frame<std::vector<uint8_t>>(from, to, path, data_it_first, data_it_last);
}

template <typename InputIt>
LIBMODEM_INLINE std::vector<uint8_t> encode_frame(const address& from, const address& to, const std::vector<address>& path, InputIt data_it_first, InputIt data_it_last, uint8_t control, uint8_t pid)
{
    return encode_frame<std::vector<uint8_t>>(from, to, path, data_it_first, data_it_last, control, pid);
}

template <typename Container, typename InputIt, typename Traits>
LIBMODEM_INLINE Container encode_frame(const address& from, const address& to, const std::vector<address>& path, InputIt data_it_first, InputIt data_it_last)
{
    static constexpr uint8_t ui_frame = 0x03;
    static constexpr uint8_t pid_no_layer3 = 0xF0;
    return encode_frame<Container, InputIt, Traits>(from, to, path, data_it_first, data_it_last, ui_frame, pid_no_layer3);
}

template <typename Container, typename Traits>
LIBMODEM_INLINE Container encode_frame(const address& from, const address& to, const std::vector<address>& path, uint8_t control)
{
    // Encodes an AX.25 frame without the PID field and without payload
    //
    //  - Build header (from, to, path)
    //  - Add control field
    //  - Compute 16 bits CRC and append at the end

    Container frame;

    // Header
    std::vector<uint8_t> header = encode_header(from, to, path);
    std::copy(header.begin(), header.end(), traits_back_insert_iterator<Container, Traits>(frame));

    // Control field (always present)
    Traits::push_back(frame, control);

    // Compute 16 bits CRC
    // Append CRC at the end of the frame
    std::array<uint8_t, 2> crc = compute_crc(frame.begin(), frame.end());
    std::copy(crc.begin(), crc.end(), traits_back_insert_iterator<Container, Traits>(frame));

    return frame;
}

template <typename Container, typename Traits>
LIBMODEM_INLINE Container encode_frame_without_crc(const address& from, const address& to, const std::vector<address>& path, uint8_t control)
{
    // Encodes an AX.25 frame without the PID field, without payload and without CRC
    //
    //  - Build header (from, to, path)
    //  - Add control field

    Container frame;

    // Header
    std::vector<uint8_t> header = encode_header(from, to, path);
    std::copy(header.begin(), header.end(), traits_back_insert_iterator<Container, Traits>(frame));

    // Control field (always present)
    Traits::push_back(frame, control);

    return frame;
}

template <typename Container, typename InputIt, typename Traits>
LIBMODEM_INLINE Container encode_frame(const address& from, const address& to, const std::vector<address>& path, InputIt data_it_first, InputIt data_it_last, uint8_t control, uint8_t pid)
{
    // Encodes an AX.25 frame
    //
    //  - Build header (from, to, path)
    //  - Add control field, and PID field if applicable
    //  - Append payload if applicable
    //  - Compute 16 bits CRC and append at the end
    //
    // PID and info field are present for:
    //  - I-frames: (control & 0x01) == 0
    //  - UI frames: (control & 0xEF) == 0x03 (UI with P/F bit masked out)

    Container frame;

    // Header
    std::vector<uint8_t> header = encode_header(from, to, path);
    std::copy(header.begin(), header.end(), traits_back_insert_iterator<Container, Traits>(frame));

    // Control field (always present)
    Traits::push_back(frame, control);

    // If I or UI frame, add PID and payload
    frame_type type = get_frame_type(control);
    bool is_i_or_ui_frame = (type == frame_type::i || type == frame_type::ui);
    if (is_i_or_ui_frame)
    {
        // PID field
        Traits::push_back(frame, pid);

        // Append payload
        std::copy(data_it_first, data_it_last, traits_back_insert_iterator<Container, Traits>(frame));
    }

    // Compute 16 bits CRC
    // Append CRC at the end of the frame
    std::array<uint8_t, 2> crc = compute_crc(frame.begin(), frame.end());
    std::copy(crc.begin(), crc.end(), traits_back_insert_iterator<Container, Traits>(frame));

    return frame;
}

template <typename ForwardIt1, typename InputIt, typename ForwardIt2>
LIBMODEM_INLINE ForwardIt2 encode_frame(const address& from, const address& to, ForwardIt1 path_first_it, ForwardIt1 path_last_it, InputIt data_it_first, InputIt data_it_last, ForwardIt2 out)
{
    static constexpr uint8_t ui_frame = 0x03;
    static constexpr uint8_t pid_no_layer3 = 0xF0;
    return encode_frame(from, to, path_first_it, path_last_it, data_it_first, data_it_last, ui_frame, pid_no_layer3, out);
}

template <typename ForwardIt1, typename ForwardIt2>
LIBMODEM_INLINE ForwardIt2 encode_frame(const address& from, const address& to, ForwardIt1 path_first_it, ForwardIt1 path_last_it, uint8_t control, ForwardIt2 out)
{
    // Encodes an AX.25 frame without the PID field and without payload
    //
    //  - Build header (from, to, path)
    //  - Add control field

    ForwardIt2 frame_start = out;
    ForwardIt2 frame_end;

    // Encoding header
    out = encode_header(from, to, path_first_it, path_last_it, out);

    // Control field (always present)
    *out++ = control;

    frame_end = out;

    // Compute 16 bits CRC
    // Append CRC at the end of the frame
    std::array<uint8_t, 2> crc = compute_crc(frame_start, frame_end);
    out = std::copy(crc.begin(), crc.end(), out);

    return out;
}

template <typename ForwardIt, typename OutputIt>
LIBMODEM_INLINE OutputIt encode_frame_without_crc(const address& from, const address& to, ForwardIt path_first_it, ForwardIt path_last_it, uint8_t control, OutputIt out)
{
    // Encodes an AX.25 frame without the PID field, without payload and without CRC
    //
    //  - Build header (from, to, path)
    //  - Add control field

    // Encoding header
    out = encode_header(from, to, path_first_it, path_last_it, out);

    // Control field (always present)
    *out++ = control;

    return out;
}

template <typename ForwardIt1, typename InputIt, typename ForwardIt2>
LIBMODEM_INLINE ForwardIt2 encode_frame(const address& from, const address& to, ForwardIt1 path_first_it, ForwardIt1 path_last_it, InputIt data_it_first, InputIt data_it_last, uint8_t control, uint8_t pid, ForwardIt2 out)
{
    // Encodes an AX.25 frame
    //
    //  - Build header (from, to, path)
    //  - Add control field, and PID field if applicable
    //  - Append payload if applicable
    //  - Compute 16 bits CRC and append at the end

    ForwardIt2 frame_start = out;
    ForwardIt2 frame_end;

    // Encoding header
    out = encode_header(from, to, path_first_it, path_last_it, out);

    // Control field (always present)
    *out++ = control;

    // If I or UI frame, add PID and payload
    frame_type type = get_frame_type(control);
    bool is_i_or_ui_frame = (type == frame_type::i || type == frame_type::ui);
    if (is_i_or_ui_frame)
    {
        // PID field
        *out++ = pid;

        // Append payload
        out = std::copy(data_it_first, data_it_last, out);
    }

    frame_end = out;

    // Compute 16 bits CRC
    // Append CRC at the end of the frame
    std::array<uint8_t, 2> crc = compute_crc(frame_start, frame_end);
    out = std::copy(crc.begin(), crc.end(), out);

    return out;
}

template <typename ForwardIt>
LIBMODEM_INLINE ForwardIt encode_frame(const packet& p, ForwardIt out)
{
    address to_address;
    LIBMODEM_NAMESPACE_REFERENCE try_parse_address(p.to, to_address);

    address from_address;
    LIBMODEM_NAMESPACE_REFERENCE try_parse_address(p.from, from_address);

    std::vector<address> path;
    for (const auto& address_string : p.path)
    {
        address path_address;
        LIBMODEM_NAMESPACE_REFERENCE try_parse_address(address_string, path_address);
        path.push_back(path_address);
    }

    out = encode_frame(from_address, to_address, path.begin(), path.end(), p.data.begin(), p.data.end(), out);

    return out;
}

template<class InputIt>
LIBMODEM_INLINE bool try_decode_packet(InputIt frame_it_first, InputIt frame_it_last, packet& p)
{
    // Decode an AX.25 packet from an NRZI bitstream
    // The frame inside the bitstream is set between frame_it_first and frame_it_last
    // There should be no HDLC flags in the frame bitstream
    // The bitstream is assumed to be NRZI decoded already

    struct frame frame;
    bool result = try_decode_frame(frame_it_first, frame_it_last, frame);
    if (result)
    {
        p = to_packet(frame);
    }
    return result;
}

template<class InputIt>
LIBMODEM_INLINE bool try_decode_frame(InputIt frame_it_first, InputIt frame_it_last, struct frame& frame) // iterator
{
    // Decode an AX.25 frame from an NRZI bitstream
    // The frame inside the bitstream is set between frame_it_first and frame_it_last
    // There should be no HDLC flags in the frame bitstream
    // The bitstream is assumed to be NRZI decoded already

    std::vector<uint8_t> unstuffed_bits;

    bit_unstuff(frame_it_first, frame_it_last, std::back_inserter(unstuffed_bits));

    std::vector<uint8_t> frame_bytes;

    bits_to_bytes(unstuffed_bits.begin(), unstuffed_bits.end(), std::back_inserter(frame_bytes));

    return try_decode_frame(frame_bytes, frame);
}

template<typename RandomIt>
LIBMODEM_INLINE bool try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data, std::array<uint8_t, 2>& crc)
{
    // Decode an AX.25 frame from a byte range

    path.clear();
    data.clear();

    auto [path_out_it, data_out_it, result] = try_decode_frame(frame_it_first, frame_it_last, from, to, std::back_inserter(path), std::back_inserter(data), crc);

    return result;
}

template<typename RandomIt>
LIBMODEM_INLINE bool try_decode_frame_no_fcs(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data)
{
    // Decode an AX.25 frame from a byte range

    path.clear();
    data.clear();

    auto [path_out_it, data_out_it, result] = try_decode_frame_no_fcs(frame_it_first, frame_it_last, from, to, std::back_inserter(path), std::back_inserter(data));

    return result;
}

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, std::array<uint8_t, 2>& crc)
{
    // Decode an AX.25 frame from a byte range
    //
    // frame_it_first and frame_it_last - iterators to the start and end of the frame byte range
    // data - contains an output iterator to the data payload of the frame
    // path - contains an output iterator to the path addresses of the frame, each address is of the type "struct address"

    uint8_t control = 0;
    uint8_t pid = 0;
    return try_decode_frame(frame_it_first, frame_it_last, from, to, path, data, control, pid, crc);
}

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame_no_fcs(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data)
{
    // Decode an AX.25 frame from a byte range
    //
    // frame_it_first and frame_it_last - iterators to the start and end of the frame byte range
    // data - contains an output iterator to the data payload of the frame
    // path - contains an output iterator to the path addresses of the frame, each address is of the type "struct address"

    uint8_t control = 0;
    uint8_t pid = 0;
    return try_decode_frame_no_fcs(frame_it_first, frame_it_last, from, to, path, data, control, pid);
}

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& crc)
{
    std::array<uint8_t, 2> expected_crc;
    (void)expected_crc;
    return try_decode_frame(frame_it_first, frame_it_last, from, to, path, data, control, pid, crc, expected_crc);
}

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    constexpr size_t max_data_length = SIZE_MAX;
    return try_decode_frame(frame_it_first, frame_it_last, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
}

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    size_t frame_size = std::distance(frame_it_first, frame_it_last);

    if (frame_size < 18)
    {
        return { path, data, false };
    }

    actual_crc = compute_crc_using_lut(frame_it_first, frame_it_last - 2);
    expected_crc = { *(frame_it_last - 2), *(frame_it_last - 1) };

    // Check CRC validity
    if (actual_crc != expected_crc)
    {
        return { path, data, false };
    }

    return try_decode_frame_no_fcs(frame_it_first, frame_it_last - 2, from, to, path, data, max_data_length, control, pid);
}

template<typename InputIt, typename ForwardIt, typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(from_nrzi_bits_t, InputIt frame_it_first, InputIt frame_it_last, ForwardIt unstuffed_bits_it, RandomIt frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    constexpr size_t max_data_length = SIZE_MAX;
    return try_decode_frame(from_nrzi_bits, frame_it_first, frame_it_last, unstuffed_bits_it, frame_bytes_it, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
}

template<typename InputIt, typename ForwardIt, typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame(from_nrzi_bits_t, InputIt frame_it_first, InputIt frame_it_last, ForwardIt unstuffed_bits_it, RandomIt frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    auto unstuffed_last_it = bit_unstuff(frame_it_first, frame_it_last, unstuffed_bits_it);
    auto bytes_last_it = bits_to_bytes(unstuffed_bits_it, unstuffed_last_it, frame_bytes_it);
    return try_decode_frame(frame_bytes_it, bytes_last_it, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
}

template<typename InputIt, typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<RandomIt, OutputIt1, OutputIt2, bool> try_decode_frame(from_nrzi_bits_t, InputIt frame_it_first, InputIt frame_it_last, RandomIt frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    constexpr size_t max_data_length = SIZE_MAX;
    return try_decode_frame(from_nrzi_bits, frame_it_first, frame_it_last, frame_bytes_it, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
}

template<typename InputIt, typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<RandomIt, OutputIt1, OutputIt2, bool> try_decode_frame(from_nrzi_bits_t, InputIt frame_it_first, InputIt frame_it_last, RandomIt frame_bytes_it, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid, std::array<uint8_t, 2>& actual_crc, std::array<uint8_t, 2>& expected_crc)
{
    auto bytes_last_it = bit_unstuff_to_bytes(frame_it_first, frame_it_last, frame_bytes_it);
    auto [path_out, data_out, result] = try_decode_frame(frame_bytes_it, bytes_last_it, from, to, path, data, max_data_length, control, pid, actual_crc, expected_crc);
    return { bytes_last_it, path_out, data_out, result };
}

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame_no_fcs(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, uint8_t& control, uint8_t& pid)
{
    constexpr size_t max_data_length = SIZE_MAX;
    return try_decode_frame_no_fcs(frame_it_first, frame_it_last, from, to, path, data, max_data_length, control, pid);
}

template<typename RandomIt, typename OutputIt1, typename OutputIt2>
LIBMODEM_INLINE std::tuple<OutputIt1, OutputIt2, bool> try_decode_frame_no_fcs(RandomIt frame_it_first, RandomIt frame_it_last, address& from, address& to, OutputIt1 path, OutputIt2 data, size_t max_data_length, uint8_t& control, uint8_t& pid)
{
    // Decode an AX.25 frame from a byte range
    //
    // frame_it_first and frame_it_last - iterators to the start and end of the frame byte range
    // data - contains an output iterator to the data payload of the frame
    // path - contains an output iterator to the path addresses of the frame, each address is of the type "struct address"

    size_t frame_size = std::distance(frame_it_first, frame_it_last);

    // Minimum: 14 (addresses) + 1[2] (control,[pid]) = 15
    if (frame_size < 15)
    {
        return { path, data, false };
    }

    // Preserve the C-bit in source/destination for test purposes, even if it makes a from/to address set as marked

    LIBMODEM_AX25_NAMESPACE_REFERENCE try_parse_address(frame_it_first, frame_it_first + 7, to);
    LIBMODEM_AX25_NAMESPACE_REFERENCE try_parse_address(frame_it_first + 7, frame_it_first + 14, from);

    // Set the C/R-bit on the to/from addresses based on the mark bit
    // Reset the mark bit to false since it is only applicable to path addresses
    to.command_response = to.mark;
    to.mark = false;
    from.command_response = from.mark;
    from.mark = false;

    size_t addresses_start = 14;
    size_t addresses_end_position = addresses_start;
    size_t addresses_count = 0;

    bool found_last_address = false;

    // Check if there are no path addresses (i.e., the source address is the last address)
    if (*(frame_it_first + 13) & 0b00000001)
    {
        found_last_address = true;
    }

    // Parse path addresses until we find the last address
    // Each address is 7 bytes long
    // The last address is indicated by the extension bit (bit 0 of byte 6) being set
    // AX.25 allows up to 8 digipeaters (10 total addresses)
    for (size_t i = addresses_start; !found_last_address && i + 7 <= frame_size - 1 && addresses_count < 8; i += 7)
    {
        addresses_count++;

        // Check if the extension bit (bit 0 of byte 6) is set, marking the last address
        if (*(frame_it_first + i + 6) & 0b00000001)
        {
            addresses_end_position = i + 7;
            found_last_address = true;
        }
    }

    if (!found_last_address)
    {
        return { path, data, false };
    }

    size_t addresses_length = addresses_end_position - addresses_start;

    // Ensure that the addresses length is a multiple of 7
    if (addresses_length % 7 != 0)
    {
        return { path, data, false };
    }

    if (addresses_length > 0)
    {
        path = parse_addresses(frame_it_first + addresses_start, frame_it_first + addresses_end_position, path);
    }

    control = *(frame_it_first + addresses_end_position);

    size_t info_field_start = addresses_end_position + 1; // skip the Control Field byte

    // PID and info field depend on frame type.
    // Only I-frames and UI frames carry a PID byte.
    frame_type type = get_frame_type(control);
    bool is_i_or_ui_frame = (type == frame_type::i || type == frame_type::ui);
    if (is_i_or_ui_frame)
    {
        if (info_field_start >= frame_size)
        {
            return { path, data, false };
        }

        pid = *(frame_it_first + addresses_end_position + 1);

        info_field_start++;
    }
    else
    {
        pid = 0;
    }

    // Check bounds before calculating length so that we do not underflow
    if (info_field_start > frame_size)
    {
        return { path, data, false };
    }

    size_t info_field_length = frame_size - info_field_start;

    if (info_field_length > max_data_length)
    {
        return { path, data, false };
    }

    if (info_field_length > 0)
    {
        data = std::copy(frame_it_first + info_field_start, frame_it_first + info_field_start + info_field_length, data);
    }

    return { path, data, true };
}

template<typename OutputIt>
LIBMODEM_INLINE OutputIt encode_header(const address& from, const address& to, const std::vector<address>& path, OutputIt out)
{
    return encode_header(from, to, path.begin(), path.end(), out);
}

template<typename ForwardIt, typename OutputIt>
LIBMODEM_INLINE OutputIt encode_header(const address& from, const address& to, ForwardIt path_first_it, ForwardIt path_last_it, OutputIt out)
{
    std::array<uint8_t, 7> to_bytes = encode_address(to, false);

    out = std::copy(to_bytes.begin(), to_bytes.end(), out);

    size_t path_size = std::distance(path_first_it, path_last_it);

    // If there is no path, the from address is the last address
    // and should be marked as such
    std::array<uint8_t, 7> from_bytes = encode_address(from, (path_size == 0));

    out = std::copy(from_bytes.begin(), from_bytes.end(), out);

    return encode_addresses(path_first_it, path_last_it, out);
}

template<typename OutputIt>
LIBMODEM_INLINE OutputIt encode_addresses(const std::vector<address>& path, OutputIt out)
{
    for (size_t i = 0; i < path.size(); i++)
    {
        bool last = (i == path.size() - 1);
        std::array<uint8_t, 7> address_bytes = encode_address(path[i], last);
        out = std::copy(address_bytes.begin(), address_bytes.end(), out);
    }

    return out;
}

template<typename ForwardIt, typename OutputIt>
LIBMODEM_INLINE OutputIt encode_addresses(ForwardIt path_first_it, ForwardIt path_last_it, OutputIt out)
{
    if (path_first_it == path_last_it)
    {
        return out;
    }

    for (auto it = path_first_it; it != path_last_it; ++it)
    {
        auto next = std::next(it);
        bool last = (next == path_last_it);
        std::array<uint8_t, 7> address_bytes = encode_address(*it, last);
        out = std::copy(address_bytes.begin(), address_bytes.end(), out);
    }

    return out;
}

template<typename InputIt>
LIBMODEM_INLINE std::vector<uint8_t> encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags)
{
    return encode_bitstream<std::vector<uint8_t>>(frame_it_first, frame_it_last, 0, preamble_flags, postamble_flags);
}

template<typename InputIt>
LIBMODEM_INLINE std::vector<uint8_t> encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    return encode_bitstream<std::vector<uint8_t>>(frame_it_first, frame_it_last, initial_nrzi_level, preamble_flags, postamble_flags);
}

template<typename InputIt>
LIBMODEM_INLINE std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags)
{
    return encode_bitstream<std::vector<uint8_t>>(nrz_scrambled, frame_it_first, frame_it_last, 0, preamble_flags, postamble_flags);
}

template<typename InputIt>
LIBMODEM_INLINE std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    return encode_bitstream<std::vector<uint8_t>>(nrz_scrambled, frame_it_first, frame_it_last, initial_nrzi_level, preamble_flags, postamble_flags);
}

template <typename Container, typename InputIt, typename Traits>
LIBMODEM_INLINE Container encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags)
{
    return encode_bitstream<Container, InputIt, Traits>(frame_it_first, frame_it_last, 0, preamble_flags, postamble_flags);
}

template <typename Container, typename InputIt, typename Traits>
LIBMODEM_INLINE Container encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    // Encode an AX.25 frame into a complete bitstream ready for modulation
    //
    // Steps:
    // 
    //  - Convert frame bytes to bits LSB-first
    //  - Bit-stuff the bits
    //  - Add HDLC flags (0x7E) at start
    //  - Add the stuffed bits
    //  - Add HDLC flags (0x7E) at end
    //  - NRZI encode the entire bitstream

    Container frame_bits;

    bytes_to_bits(frame_it_first, frame_it_last, traits_back_insert_iterator<Container, Traits>(frame_bits));

    // Bit stuffing

    Container stuffed_bits;

    bit_stuff(frame_bits.begin(), frame_bits.end(), traits_back_insert_iterator<Container, Traits>(stuffed_bits));

    // Build complete bitstream: preamble + data + postamble

    Container bitstream;

    add_hdlc_flags(traits_back_insert_iterator<Container, Traits>(bitstream), preamble_flags);
    std::copy(stuffed_bits.begin(), stuffed_bits.end(), traits_back_insert_iterator<Container, Traits>(bitstream));
    add_hdlc_flags(traits_back_insert_iterator<Container, Traits>(bitstream), postamble_flags);

    // NRZI encoding of the bitstream

    nrzi_encode(bitstream.begin(), bitstream.end(), initial_nrzi_level);

    return bitstream;
}

template <typename Container, typename InputIt, typename Traits>
LIBMODEM_INLINE Container encode_bitstream(nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags)
{
    return encode_bitstream<Container, InputIt, Traits>(nrz_scrambled, frame_it_first, frame_it_last, 0, preamble_flags, postamble_flags);
}

template <typename Container, typename InputIt, typename Traits>
LIBMODEM_INLINE Container encode_bitstream(nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    Container frame_bits;

    bytes_to_bits(frame_it_first, frame_it_last, traits_back_insert_iterator<Container, Traits>(frame_bits));

    Container stuffed_bits;

    bit_stuff(frame_bits.begin(), frame_bits.end(), traits_back_insert_iterator<Container, Traits>(stuffed_bits));

    Container bitstream;

    add_hdlc_flags(traits_back_insert_iterator<Container, Traits>(bitstream), preamble_flags);
    std::copy(stuffed_bits.begin(), stuffed_bits.end(), traits_back_insert_iterator<Container, Traits>(bitstream));
    add_hdlc_flags(traits_back_insert_iterator<Container, Traits>(bitstream), postamble_flags);

    nrzi_encode(bitstream.begin(), bitstream.end(), initial_nrzi_level);

    scramble_bits(bitstream.begin(), bitstream.end());

    return bitstream;
}

template<typename InputIt, typename ForwardIt>
LIBMODEM_INLINE ForwardIt encode_bitstream(InputIt frame_first, InputIt frame_last, int preamble_flags, int postamble_flags, ForwardIt out)
{
    return encode_bitstream(frame_first, frame_last, 0, preamble_flags, postamble_flags, out);
}

template<typename InputIt, typename ForwardIt>
LIBMODEM_INLINE ForwardIt encode_bitstream(InputIt frame_first, InputIt frame_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags, ForwardIt out)
{
    ForwardIt begin = out;

    // Preamble
    out = add_hdlc_flags(out, preamble_flags);

    // Frame bytes to bits
    out = bytes_to_bits_stuffed(frame_first, frame_last, out);

    // Postamble
    out = add_hdlc_flags(out, postamble_flags);

    // NRZI encode entire bitstream in-place
    nrzi_encode(begin, out, initial_nrzi_level);

    return out;
}

template<typename InputIt, typename ForwardIt>
ForwardIt encode_bitstream(nrz_scrambled_t, InputIt frame_first, InputIt frame_last, int preamble_flags, int postamble_flags, ForwardIt out)
{
    return encode_bitstream(nrz_scrambled, frame_first, frame_last, 0, preamble_flags, postamble_flags, out);
}

template<typename InputIt, typename ForwardIt>
ForwardIt encode_bitstream(nrz_scrambled_t, InputIt frame_first, InputIt frame_last, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags, ForwardIt out)
{
    ForwardIt begin = out;

    // Preamble
    out = add_hdlc_flags(out, preamble_flags);

    // Frame bytes to bits
    out = bytes_to_bits_stuffed(frame_first, frame_last, out);

    // Postamble
    out = add_hdlc_flags(out, postamble_flags);

    // NRZI encode entire bitstream in-place
    nrzi_encode(begin, out, initial_nrzi_level);

    // Scramble
    scramble_bits(begin, out);

    return out;
}

LIBMODEM_AX25_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// FX.25                                                            //
//                                                                  //
// encode_frame, encode_bitstream                                   //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_FX25_NAMESPACE_BEGIN

std::vector<uint8_t> encode_frame(const std::vector<uint8_t>& frame_bytes, size_t min_check_bytes = 0);

std::vector<uint8_t> encode_frame(std::span<const uint8_t> frame_bytes, size_t min_check_bytes);

template<typename InputIt>
std::vector<uint8_t> encode_frame(InputIt frame_it_first, InputIt frame_it_last, size_t min_check_bytes);

template<typename InputIt>
std::vector<uint8_t> encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags, size_t min_check_bytes = 0);

std::vector<uint8_t> encode_bitstream(const packet& p, int preamble_flags, int postamble_flags, size_t min_check_bytes = 0);
std::vector<uint8_t> encode_bitstream(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags, size_t min_check_bytes = 0);

template<typename InputIt>
LIBMODEM_INLINE std::vector<uint8_t> encode_frame(InputIt frame_it_first, InputIt frame_it_last, size_t min_check_bytes)
{
    return encode_frame(std::span<const uint8_t>{ frame_it_first, frame_it_last }, min_check_bytes);
}

template<typename InputIt>
LIBMODEM_INLINE std::vector<uint8_t> encode_bitstream(InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags, size_t min_check_bytes)
{
LIBMODEM_AX25_USING_NAMESPACE

    // Encode FX.25 frame
    // 
    //  - Convert AX.25 frame to bits LSB-first
    //  - Bit-stuff the bits
    //  - Add HDLC flags (0x7E) at start
    //  - Add the stuffed bits
    //  - Add HDLC flags (0x7E) at end
    //  - Create FX.25 frame from stuffed bits containing the HDLC flags
    //  - Convert FX.25 frame to bits LSB-first
    //  - Add HDLC flags (0x7E) at start
    //  - Add the FX.25 frame bits
    //  - Add HDLC flags (0x7E) at end
    //  - NRZI encode the entire bitstream
        
    std::vector<uint8_t> frame_bits;

    bytes_to_bits(frame_it_first, frame_it_last, std::back_inserter(frame_bits));

    std::vector<uint8_t> stuffed_bits;

    bit_stuff(frame_bits.begin(), frame_bits.end(), std::back_inserter(stuffed_bits));

    // Build complete AX.25 frame bits: preamble + stuffed bits + postamble

    std::vector<uint8_t> ax25_bits;

    add_hdlc_flags(std::back_inserter(ax25_bits), 1);
    ax25_bits.insert(ax25_bits.end(), stuffed_bits.begin(), stuffed_bits.end());
    add_hdlc_flags(std::back_inserter(ax25_bits), 1);

    // Create FX.25 frame

    std::vector<uint8_t> ax25_packet_bytes;

    bits_to_bytes(ax25_bits.begin(), ax25_bits.end(), std::back_inserter(ax25_packet_bytes));

    std::vector<uint8_t> fx25_frame = encode_frame(ax25_packet_bytes, min_check_bytes);

    if (fx25_frame.empty()) 
    {
        return {};
    }

    // Build complete bitstream: preamble + data + postamble

    std::vector<uint8_t> bitstream;

    add_hdlc_flags(std::back_inserter(bitstream), preamble_flags);
    bytes_to_bits(fx25_frame.begin(), fx25_frame.end(), std::back_inserter(bitstream));
    add_hdlc_flags(std::back_inserter(bitstream), postamble_flags);

    // NRZI encoding of the bitstream

    nrzi_encode(bitstream.begin(), bitstream.end());

    return bitstream;
}

std::vector<uint8_t> encode_bitstream(const packet& p, int preamble_flags, int postamble_flags, size_t min_check_bytes);
std::vector<uint8_t> encode_bitstream(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags, size_t min_check_bytes);

std::vector<uint8_t> encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled_t, const packet& p, int preamble_flags, int postamble_flags, size_t min_check_bytes = 0);
std::vector<uint8_t> encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled_t, const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags, size_t min_check_bytes = 0);

template<typename InputIt>
std::vector<uint8_t> encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags, size_t min_check_bytes = 0);

template<typename InputIt>
LIBMODEM_INLINE std::vector<uint8_t> encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled_t, InputIt frame_it_first, InputIt frame_it_last, int preamble_flags, int postamble_flags, size_t min_check_bytes)
{
LIBMODEM_AX25_USING_NAMESPACE

    // Encode FX.25 frame with NRZ scrambling
    //
    //  - Convert AX.25 frame to bits LSB-first
    //  - Bit-stuff the bits
    //  - Add HDLC flags (0x7E) at start
    //  - Add the stuffed bits
    //  - Add HDLC flags (0x7E) at end
    //  - Create FX.25 frame from stuffed bits containing the HDLC flags
    //  - Convert FX.25 frame to bits LSB-first
    //  - Add HDLC flags (0x7E) at start
    //  - Add the FX.25 frame bits
    //  - Add HDLC flags (0x7E) at end
    //  - NRZI encode the entire bitstream
    //  - Scramble the entire bitstream

    std::vector<uint8_t> frame_bits;

    bytes_to_bits(frame_it_first, frame_it_last, std::back_inserter(frame_bits));

    std::vector<uint8_t> stuffed_bits;

    bit_stuff(frame_bits.begin(), frame_bits.end(), std::back_inserter(stuffed_bits));

    // Build complete AX.25 frame bits: preamble + stuffed bits + postamble

    std::vector<uint8_t> ax25_bits;

    add_hdlc_flags(std::back_inserter(ax25_bits), 1);
    ax25_bits.insert(ax25_bits.end(), stuffed_bits.begin(), stuffed_bits.end());
    add_hdlc_flags(std::back_inserter(ax25_bits), 1);

    // Create FX.25 frame

    std::vector<uint8_t> ax25_packet_bytes;

    bits_to_bytes(ax25_bits.begin(), ax25_bits.end(), std::back_inserter(ax25_packet_bytes));

    std::vector<uint8_t> fx25_frame = encode_frame(ax25_packet_bytes, min_check_bytes);

    if (fx25_frame.empty())
    {
        return {};
    }

    // Build complete bitstream: preamble + data + postamble

    std::vector<uint8_t> bitstream;

    add_hdlc_flags(std::back_inserter(bitstream), preamble_flags);
    bytes_to_bits(fx25_frame.begin(), fx25_frame.end(), std::back_inserter(bitstream));
    add_hdlc_flags(std::back_inserter(bitstream), postamble_flags);

    // NRZI encoding of the bitstream

    nrzi_encode(bitstream.begin(), bitstream.end());

    // Scramble the bitstream

    scramble_bits(bitstream.begin(), bitstream.end());

    return bitstream;
}

std::vector<uint8_t> encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled_t, const packet& p, int preamble_flags, int postamble_flags, size_t min_check_bytes);
std::vector<uint8_t> encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled_t, const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags, size_t min_check_bytes);

LIBMODEM_FX25_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// IL2P                                                             //
//                                                                  //
// encode_frame, encode_bitstream                                   //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_IL2P_NAMESPACE_BEGIN

std::vector<uint8_t> encode_frame(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f);
std::vector<uint8_t> encode_frame(const packet& p);

std::vector<uint8_t> encode_bitstream(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags = 1, int postamble_flags = 1);
std::vector<uint8_t> encode_bitstream(const packet& p, int preamble_flags = 1, int postamble_flags = 1);

LIBMODEM_IL2P_NAMESPACE_END

LIBMODEM_NAMESPACE_END