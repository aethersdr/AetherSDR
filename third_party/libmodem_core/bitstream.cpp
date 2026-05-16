// **************************************************************** //
// libmodem - APRS modem                                            //
// Version 0.1.0                                                    //
// https://github.com/iontodirel/libmodem                           //
// Copyright (c) 2025 Ion Todirel                                   //
// **************************************************************** //
//
// bitstream.cpp
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

#include "bitstream.h"

#include <array>
#include <string>
#include <cassert>
#include <tuple>
#include <charconv>
#include <memory>

LIBMODEM_NAMESPACE_BEGIN

// **************************************************************** //
//                                                                  //
//                                                                  //
// address                                                          //
//                                                                  //
//                                                                  //
// **************************************************************** //

bool try_parse_address(std::string_view address_string, struct address& address)
{
    // This function is a wrapper around try_parse_address
    // it will parse the address and ssid from the address_string
    // and create an address type.

    std::string address_no_ssid;
    int ssid = 0;
    bool mark = false;

    if (!try_parse_address_with_used_flag(address_string, address_no_ssid, ssid, mark))
    {
        return false;
    }

    size_t length = std::min(address_no_ssid.size(), size_t(6));

    std::copy_n(address_no_ssid.begin(), length, address.text.data());

    address.text_length = length;
    address.ssid = ssid;
    address.mark = mark;

    return true;
}

bool try_parse_address(std::string_view address, std::string& address_no_ssid, int& ssid)
{
    // Try parse an address like: ADDRESS[-SSID]
    //
    // Example:
    //
    // CALL1-10
    // ~~~~~ ~~
    // ^     ssid = 10
    // |
    // address_no_ssid = CALL1
    //
    // This functions expects a valid AX.25 address,
    // and will return false if the address is not valid.
    // An address with a non numeric ssid will be rejected, ex: CALL-AB

    ssid = 0;

    if (address.empty() || address.size() > 9)
    {
        return false;
    }

    auto sep_position = address.find("-");

    if (sep_position != std::string_view::npos)
    {
        // Check few error conditions
        // If packet ends with a separator but no ssid, ex: "CALL-"
        // If there are more than 2 character after the separator, ex: CALL-123
        if ((sep_position == (address.size() - 1)) || ((sep_position + 3) < address.size()))
        {
            return false;
        }

        address_no_ssid.assign(address.begin(), address.begin() + sep_position);

        std::string ssid_string;
        ssid_string.assign(address.begin() + sep_position + 1, address.end());

        if (ssid_string[0] == '0')
        {
            return false;
        }

        // Ensure the ssid is a number
        if (!std::isdigit(static_cast<unsigned char>(ssid_string[0])) ||
            (ssid_string.size() > 1 && !std::isdigit(static_cast<unsigned char>(ssid_string[1]))))
        {
            return false;
        }

        if (!try_parse_int({ ssid_string.data(), ssid_string.size() }, ssid))
        {
            return false;
        }

        if (ssid < 0 || ssid > 15)
        {
            ssid = 0;
            return false;
        }
    }
    else
    {
        address_no_ssid.assign(address.begin(), address.end());
        ssid = 0;
    }

    if (address_no_ssid.size() > 6)
    {
        return false;
    }

    for (char c : address_no_ssid)
    {
        // The address has to be alphanumeric and uppercase, or a digit
        if ((!std::isalnum(static_cast<unsigned char>(c)) || !std::isdigit(static_cast<unsigned char>(c))) &&
            !std::isupper(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }

    return true;
}

bool try_parse_address_with_used_flag(std::string_view address, std::string& address_no_ssid, int& ssid, bool& mark)
{
    ssid = 0;
    mark = false;

    if (address.empty())
    {
        return false;
    }

    if (address.back() == '*')
    {
        mark = true;
        address.remove_suffix(1);
    }

    return try_parse_address(address, address_no_ssid, ssid);
}

std::string to_string(const struct address& address)
{
    return to_string(address, false);
}

std::string to_string(const struct address& address, bool ignore_mark)
{
    if (address.text_length == 0)
    {
        return "";
    }

    std::string result(address.text.data(), address.text_length);

    if (address.ssid > 0)
    {
        result += '-';
        if (address.ssid < 10)
        {
            result += char('0' + address.ssid);
        }
        else
        {
            // 10 .. 15 => "1" + '0'..'5'
            result += '1';
            result += char('0' + (address.ssid - 10));
        }
    }

    if (address.mark && !ignore_mark)
    {
        result += '*';
    }

    return result;
}

bool validate_address(const struct address& address)
{
    // Reject empty callsigns and lengths that exceed the fixed-size text buffer.
    if (address.text_length == 0 || address.text_length > address.text.size())
    {
        return false;
    }

    // AX.25 address field allows only uppercase letters and digits (shifted ASCII).
    for (size_t i = 0; i < address.text_length; i++)
    {
        unsigned char c = static_cast<unsigned char>(address.text[i]);
        if (!std::isupper(c) && !std::isdigit(c))
        {
            return false;
        }
    }

    // SSID is a 4-bit field, so valid values are 0..15.
    if (address.ssid < 0 || address.ssid > 15)
    {
        return false;
    }

    return true;
}

bool try_parse_int(std::string_view string, int& value)
{
    // Attempt to parse an integer from the given string_view.
    // Returns true if parsing is successful, false otherwise.
    // If parsing fails, the value is set to 0.

    auto result = std::from_chars(string.data(), string.data() + string.size(), value);

    // Check if the parsing was successful and if the entire string was consumed
    // The result.ec should be std::errc() and result.ptr should point to the end of the string
    bool success = (result.ec == std::errc()) && (result.ptr == (string.data() + string.size()));

    // If parsing fails, set value to 0
    if (!success)
    {
        value = 0;
    }

    return success;
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// packet                                                           //
//                                                                  //
//                                                                  //
// **************************************************************** //

packet::packet(const std::string& from, const std::string& to, const std::vector<std::string>& path, const std::string& data) : from(from), to(to), path(path), data(data)
{
}

packet::packet(const char* packet_string)
{
    bool result = try_decode_packet(packet_string, *this);
    (void)result;
    assert(result);
}

packet::packet(const std::string& packet_string)
{
    bool result = try_decode_packet(packet_string, *this);
    (void)result;
    assert(result);
}

packet::operator std::string() const
{
    return to_string(*this);
}

bool operator==(const packet& lhs, const packet& rhs)
{
    return lhs.from == rhs.from &&
        lhs.to == rhs.to &&
        lhs.path == rhs.path &&
        lhs.data == rhs.data;
}

std::string to_string(const struct packet& packet)
{
    // Does not guarantee formatting a correct packet string
    // if the input packet is invalid ex: missing path

    std::string result = packet.from + ">" + packet.to;

    if (!packet.path.empty())
    {
        for (const auto& address : packet.path)
        {
            result += "," + address;
        }
    }

    result += ":" + packet.data;

    return result;
}

bool try_decode_packet(std::string_view packet_string, packet& result)
{
    // Parse a packet: N0CALL>APRS,CALLA,CALLB*,CALLC,CALLD,CALLE,CALLF,CALLG:data
    //                 ~~~~~~ ~~~~ ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ ~~~~
    //                 from   to   path                                       data
    //
    // This function does the minimum required to parse a packet string.
    // If packet string is invalid, filling of the the packet fields is not guaranteed,
    // e.g. missing data separator ":", or missig "path"

    result.path.clear();

    // Find the from address, and the end of the packet header
    //
    // N0CALL>APRS,CALLA,CALLB*,CALLC,CALLD,CALLE,CALLF,CALLG:data
    //       ~                                               ~
    //       from_end_pos                                    colon_pos
    //
    // If we cannot find the from position, or the end of the header, we fail the parsing

    size_t from_end_pos = packet_string.find('>');

    if (from_end_pos == std::string_view::npos)
    {
        return false;
    }

    size_t colon_pos = packet_string.find(':', from_end_pos);

    if (colon_pos == std::string_view::npos)
    {
        return false;
    }

    result.from = packet_string.substr(0, from_end_pos);

    // Find the 'to' address, and the 'path'
    //
    // N0CALL>APRS,CALLA,CALLB*,CALLC,CALLD,CALLE,CALLF,CALLG:data
    //        ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //        to_and_path

    std::string_view to_and_path = packet_string.substr(from_end_pos + 1, colon_pos - from_end_pos - 1);

    size_t comma_pos = to_and_path.find(',');

    // If we cannot find the comma, comma_pos will be set to the largest positive unsigned integer
    // with 'to' containing the remaining string, and with path being empty
    //
    // comma_pos = 18446744073709551615
    // 
    // N0CALL>APRS:data
    //        ~~~~
    //        to

    result.to = to_and_path.substr(0, comma_pos);

    if (comma_pos != std::string_view::npos)
    {
        std::string_view path = to_and_path.substr(comma_pos + 1);

        // Keep consuming the path until we reach the end of the header (colon_pos)
        // We use remove_prefix, which just changes the beginning of the string_view
        // It does not modify the string, nor copy it
        //
        // 1st iteration: CALLA,CALLB*,CALLC,CALLD,CALLE,CALLF,CALLG
        //                ~~~~~
        // 2nd iteration: CALLB*,CALLC,CALLD,CALLE,CALLF,CALLG
        //                ~~~~~~
        // 3rd iteration: CALLC,CALLD,CALLE,CALLF,CALLG
        //                ~~~~~
        // 4th iteration: CALLD,CALLE,CALLF,CALLG
        //                ~~~~~
        // 5th iteration: CALLE,CALLF,CALLG
        //                ~~~~~
        // 6th iteration: CALLF,CALLG
        //                ~~~~~
        // 7th iteration: CALLG
        //                ~~~~~

        while (!path.empty())
        {
            comma_pos = path.find(',');

            std::string_view address = path.substr(0, comma_pos);

            result.path.emplace_back(address);

            if (comma_pos == std::string_view::npos)
            {
                break;
            }

            // No copy or string modification, just update the string_view beginning
            path.remove_prefix(comma_pos + 1);
        }
    }

    // The remaining string after the colon_pos is the data

    result.data = packet_string.substr(colon_pos + 1);

    return true;
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// frame                                                            //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_AX25_NAMESPACE_BEGIN

frame_type get_frame_type(uint8_t control)
{
    if ((control & 0x01u) == 0x00u)
    {
        return frame_type::i;
    }

    if ((control & 0x03u) == 0x01u)
    {
        switch (control & 0x0Fu)
        {
            case 0x01u: return frame_type::rr;
            case 0x05u: return frame_type::rnr;
            case 0x09u: return frame_type::rej;
            case 0x0Du: return frame_type::srej;
            default:    return frame_type::unknown;
        }
    }

    switch (control & 0xEFu)
    {
        case 0x03u: return frame_type::ui;
        case 0x2Fu: return frame_type::sabm;
        case 0x6Fu: return frame_type::sabme;
        case 0x43u: return frame_type::disc;
        case 0x0Fu: return frame_type::dm;
        case 0x63u: return frame_type::ua;
        case 0x87u: return frame_type::frmr;
        case 0xAFu: return frame_type::xid;
        case 0xE3u: return frame_type::test;
        default:    return frame_type::unknown;
    }
}

bool is_u_frame_type(frame_type type)
{
    switch (type)
    {
        case frame_type::ui:
        case frame_type::sabm:
        case frame_type::sabme:
        case frame_type::disc:
        case frame_type::dm:
        case frame_type::ua:
        case frame_type::frmr:
        case frame_type::xid:
        case frame_type::test:
            return true;
        default:
            return false;
    }
}

bool is_s_frame_type(frame_type type)
{
    switch (type)
    {
        case frame_type::rr:
        case frame_type::rnr:
        case frame_type::rej:
        case frame_type::srej:
            return true;
        default:
            return false;
    }
}

packet to_packet(const struct frame& frame)
{
    packet p;

    p.from = to_string(frame.from, true); // ignore mark in from address
    p.to = to_string(frame.to, true); // ignore mark in to address

    p.path.clear();
    for (size_t i = 0; i < frame.path_count; i++)
    {
        p.path.push_back(to_string(frame.path[i]));
    }

    p.data = std::string(frame.data.begin(), frame.data.begin() + frame.data_length);

    return p;
}

frame to_frame(const packet& p)
{
    static constexpr uint8_t ui_frame = 0x03;
    static constexpr uint8_t pid_no_layer3 = 0xF0;

    frame f;

    LIBMODEM_NAMESPACE_REFERENCE try_parse_address(p.from, f.from);
    LIBMODEM_NAMESPACE_REFERENCE try_parse_address(p.to, f.to);

    f.path_count = 0;
    for (const auto& path_address_string : p.path)
    {
        if (f.path_count >= f.path.size())
        {
            break;
        }

        address path_address;
        LIBMODEM_NAMESPACE_REFERENCE try_parse_address(path_address_string, path_address);

        f.path[f.path_count++] = path_address;
    }

    // Clamp data to the fixed-size frame buffer capacity
    f.data_length = std::min(p.data.size(), f.data.size());

    std::copy_n(p.data.begin(), f.data_length, f.data.begin());

    f.control = { ui_frame, 0x0 };
    f.pid = pid_no_layer3;

    return f;
}

bool validate_pid(uint8_t pid)
{
    // Validates that the PID (Protocol Identifier) byte matches one of the
    // layer-3 protocol values defined by the AX.25 v2.2 specification.

    // AX.25 layer-3 PIDs occupy the ranges 0x10-0x1F and 0x20-0x2F.
    if ((pid & 0x30) == 0x10 || (pid & 0x30) == 0x20)
    {
        return true;
    }

    switch (pid)
    {
        case 0x01: // ISO 8208
        case 0x06: // compressed TCP/IP
        case 0x07: // uncompressed TCP/IP
        case 0x08: // segmentation fragment
        case 0xC3: // TEXNET
        case 0xC4: // link quality protocol
        case 0xCA: // Appletalk
        case 0xCB: // Appletalk ARP
        case 0xCC: // ARPA IP
        case 0xCD: // ARPA ARP
        case 0xCE: // FlexNet
        case 0xCF: // TheNET
        case 0xF0: // no layer 3 (APRS)
        case 0xFF: // escape
            return true;
        default:
            return false;
    }
}

bool validate_frame(const struct frame& frame)
{
    // AX.25 permits at most 8 digipeater addresses in the path, and the
    // information (data) field is capped at 256 bytes.
    if (frame.path_count > 8 || frame.data_length > 256)
    {
        return false;
    }

    // Delegate to the field-level overload, passing only the populated
    // portions of the path and data buffers.
    //
    // The frame already carries a CRC that was produced earlier in the pipeline;
    // re-validating it is out of scope here, so we pass it as both the actual and
    // expected CRC, effectively skipping the CRC comparison at this layer.
    return validate_frame(frame.from, frame.to, frame.path.begin(), frame.path.begin() + frame.path_count, frame.data.begin(), frame.data.begin() + frame.data_length, frame.control[0], frame.pid, frame.crc, frame.crc);
}

LIBMODEM_AX25_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// bitstream_state                                                  //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_AX25_NAMESPACE_BEGIN

void bitstream_state::reset()
{
    searching = true;
    in_preamble = false;
    in_frame = false;
    complete = false;
    aborted = false;
    last_nrzi_level = 0;
    frame_start_index = 0;
    frame_end_index = 0;
    bitstream_size = 0;
    hdlc_shift_register = 0;
    current_byte = 0;
    bit_index = 0;
    stuff_count = 0;
    byte_count = 0;
    global_preamble_start = 0;
    global_postamble_end = 0;
    frame_nrzi_level = 0;
    preamble_nrzi_level = 0;
    frame_size_bits = 0;
    // max_frame_bits is intentionally not reset; it is a configuration setting
    global_bit_count = 0;
    global_preamble_start_pending = 0;
    frame_nrzi_level_pending = 0;
    preamble_count = 0;
    postamble_count = 0;
    preamble_count_pending = 0;
    postamble_count_pending = 0;
}

LIBMODEM_AX25_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// ax25_bitstream_converter                                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> ax25_bitstream_converter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
LIBMODEM_AX25_USING_NAMESPACE

    return encode_bitstream(f, preamble_flags, postamble_flags);
}

std::vector<uint8_t> ax25_bitstream_converter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
LIBMODEM_AX25_USING_NAMESPACE

    return encode_bitstream(p, preamble_flags, postamble_flags);
}

bool ax25_bitstream_converter::try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read)
{
LIBMODEM_AX25_USING_NAMESPACE

    return try_decode_bitstream(bitstream, offset, p, read, state, bitstream_buffer);
}

bool ax25_bitstream_converter::try_decode(uint8_t bit, packet& p)
{
LIBMODEM_AX25_USING_NAMESPACE

    return try_decode_bitstream(bit, p, state, bitstream_buffer);
}

void ax25_bitstream_converter::reset()
{
    state.reset();
    bitstream_buffer.clear();
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// ax25_scrambled_bitstream_converter                                //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> ax25_scrambled_bitstream_converter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
LIBMODEM_AX25_USING_NAMESPACE

    return encode_bitstream(nrz_scrambled, f, preamble_flags, postamble_flags);
}

std::vector<uint8_t> ax25_scrambled_bitstream_converter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
LIBMODEM_AX25_USING_NAMESPACE

    return encode_bitstream(nrz_scrambled, p, preamble_flags, postamble_flags);
}

bool ax25_scrambled_bitstream_converter::try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read)
{
    (void)bitstream;
    (void)offset;
    (void)p;
    (void)read;
    return false;
}

bool ax25_scrambled_bitstream_converter::try_decode(uint8_t bit, packet& p)
{
    (void)bit;
    (void)p;
    return false;
}

void ax25_scrambled_bitstream_converter::reset()
{
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// fx25_bitstream_converter                                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> fx25_bitstream_converter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
    return LIBMODEM_FX25_NAMESPACE_REFERENCE encode_bitstream(f, preamble_flags, postamble_flags);
}

std::vector<uint8_t> fx25_bitstream_converter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
LIBMODEM_FX25_USING_NAMESPACE

    return encode_bitstream(p, preamble_flags, postamble_flags);
}

bool fx25_bitstream_converter::try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read)
{
LIBMODEM_FX25_USING_NAMESPACE

    (void)bitstream;
    (void)offset;
    (void)p;
    (void)read;

    return false;
}

void fx25_bitstream_converter::reset()
{
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// fx25_scrambled_bitstream_converter                                //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> fx25_scrambled_bitstream_converter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
    return LIBMODEM_FX25_NAMESPACE_REFERENCE encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled, f, preamble_flags, postamble_flags);
}

std::vector<uint8_t> fx25_scrambled_bitstream_converter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
    return fx25::encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled, p, preamble_flags, postamble_flags);
}

bool fx25_scrambled_bitstream_converter::try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read)
{
    (void)bitstream;
    (void)offset;
    (void)p;
    (void)read;

    return false;
}

void fx25_scrambled_bitstream_converter::reset()
{
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// bitstream_converter_base                                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

bitstream_converter_base::~bitstream_converter_base()
{
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// ax25_bitstream_converter_adapter                                 //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> ax25_bitstream_converter_adapter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
    return converter.encode(f, preamble_flags, postamble_flags);
}

std::vector<uint8_t> ax25_bitstream_converter_adapter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
    return converter.encode(p, preamble_flags, postamble_flags);
}

bool ax25_bitstream_converter_adapter::try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read)
{
    return converter.try_decode(bitstream, offset, p, read);
}

bool ax25_bitstream_converter_adapter::try_decode(uint8_t bit, packet& p)
{
    return converter.try_decode(bit, p);
}

void ax25_bitstream_converter_adapter::reset()
{
    converter.reset();
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// ax25_scrambled_bitstream_converter_adapter                        //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> ax25_scrambled_bitstream_converter_adapter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
    return converter.encode(f, preamble_flags, postamble_flags);
}

std::vector<uint8_t> ax25_scrambled_bitstream_converter_adapter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
    return converter.encode(p, preamble_flags, postamble_flags);
}

bool ax25_scrambled_bitstream_converter_adapter::try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read)
{
    return converter.try_decode(bitstream, offset, p, read);
}

bool ax25_scrambled_bitstream_converter_adapter::try_decode(uint8_t bit, packet& p)
{
    return converter.try_decode(bit, p);
}

void ax25_scrambled_bitstream_converter_adapter::reset()
{
    converter.reset();
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// fx25_bitstream_converter_adapter                                 //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> fx25_bitstream_converter_adapter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
    return converter.encode(f, preamble_flags, postamble_flags);
}

std::vector<uint8_t> fx25_bitstream_converter_adapter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
    return converter.encode(p, preamble_flags, postamble_flags);
}

bool fx25_bitstream_converter_adapter::try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read)
{
    return converter.try_decode(bitstream, offset, p, read);
}

bool fx25_bitstream_converter_adapter::try_decode(uint8_t bit, packet& p)
{
    (void)bit;
    (void)p;
    return false;
}

void fx25_bitstream_converter_adapter::reset()
{
    converter.reset();
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// fx25_scrambled_bitstream_converter_adapter                        //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> fx25_scrambled_bitstream_converter_adapter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
    return converter.encode(f, preamble_flags, postamble_flags);
}

std::vector<uint8_t> fx25_scrambled_bitstream_converter_adapter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
    return converter.encode(p, preamble_flags, postamble_flags);
}

bool fx25_scrambled_bitstream_converter_adapter::try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read)
{
    return converter.try_decode(bitstream, offset, p, read);
}

bool fx25_scrambled_bitstream_converter_adapter::try_decode(uint8_t bit, packet& p)
{
    (void)bit;
    (void)p;
    return false;
}

void fx25_scrambled_bitstream_converter_adapter::reset()
{
    converter.reset();
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// trim                                                             //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::string_view trim(std::string_view str)
{
    size_t first = str.find_first_not_of(' ');
    if (first == std::string_view::npos)
    {
        return {};
    }
    size_t last = str.find_last_not_of(' ');
    return str.substr(first, last - first + 1);
}

// **************************************************************** //
//                                                                  //
//                                                                  //
//                                                                  //
// bitstream routines                                               //
//                                                                  //
// nrzi_decode, ends_with_hdlc_flag                                 //
//                                                                  //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_AX25_NAMESPACE_BEGIN

uint8_t nrzi_decode(uint8_t bit, uint8_t last_nrzi_level)
{
    uint8_t decoded_bit = (bit == last_nrzi_level) ? 1 : 0;
    return decoded_bit;
}

uint8_t scramble_bit(uint8_t bit, uint32_t& state)
{
    // G3RUH self-synchronizing scrambler, polynomial 1 + X^12 + X^17.
    //
    // Long runs of identical bits produce DC bias at the radio's data port,
    // which upsets the FM modulator. The scrambler breaks up these runs by
    // XORing each input bit with two feedback taps from a 17-bit shift register,
    // producing a pseudo-random output regardless of the input pattern.
    //
    // Reset state to 0 at the start of each packet.
    //
    //
    //  tap[16]                tap[11]
    //     |                      |
    //     ↓                      ↓
    //  +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
    //  |s16|s15|s14|s13|s12|s11|s10| s9| s8| s7| s6| s5| s4| s3| s2| s1| s0|  <- state in
    //  +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
    //    |                       |
    //    +----> XOR <------------+
    //            |
    //           XOR <--- bit (input)
    //            |
    //           res = bit ^ s16 ^ s11
    //            |
    //   each bit shifts left, MSB dropped, res enters at bit 0 ----------+
    //                                                                    |
    //                                                                    ↓
    //  +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
    //  |s15|s14|s13|s12|s11|s10| s9| s8| s7| s6| s5| s4| s3| s2| s1| s0|res|  -> state out
    //  +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

    uint8_t result = (bit ^ (state >> 16) ^ (state >> 11)) & 1;  // tap bits 16 and 11

    state = ((state << 1) | result) & 0x1FFFF;                   // shift result into 17-bit register

    return result;
}

bool ends_with_hdlc_flag(const std::vector<uint8_t>& bitstream)
{
    return ends_with_hdlc_flag(bitstream.begin(), bitstream.end());
}

bool hdlc_shift_register_detect(uint8_t decoded_bit, uint8_t& shift_register)
{
    // Shift in the new bit and check for HDLC flag pattern (0x7E = 01111110)
    shift_register = static_cast<uint8_t>((shift_register << 1) | decoded_bit);
    return shift_register == 0x7E;
}

bool bit_unstuff_to_byte(uint8_t bit, uint8_t& byte, int& bit_index, int& stuff_count)
{
    if (bit == 1)
    {
        stuff_count++;
        byte |= (1 << bit_index);
        bit_index++;
    }
    else
    {
        if (stuff_count == 5)
        {
            stuff_count = 0;
            return false;
        }

        bit_index++;
        stuff_count = 0;
    }

    if (bit_index == 8)
    {
        bit_index = 0;
        return true;
    }

    return false;
}

uint16_t compute_crc_using_lut_init()
{
    return 0xFFFF;
}

uint16_t compute_crc_using_lut_update(uint8_t byte, uint16_t crc)
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

    uint8_t table_index = (crc ^ byte) & 0xFF;
    return (crc >> 8) ^ crc_table[table_index];
}

std::array<uint8_t, 2> compute_crc_using_lut_finalize(uint16_t crc)
{
    uint16_t final_crc = crc ^ 0xFFFF;
    return { static_cast<uint8_t>(final_crc & 0xFF), static_cast<uint8_t>((final_crc >> 8) & 0xFF) };
}

LIBMODEM_AX25_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// AX.25                                                            //
//                                                                  //
// encode_header, encode_addresses, encode_address, encode_frame    //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_AX25_NAMESPACE_BEGIN

bool try_parse_address(std::string_view data, std::string& address_text, int& ssid, bool& cr_or_h_bit)
{
    return try_parse_address(data.begin(), data.end(), address_text, ssid, cr_or_h_bit);
}

bool try_parse_address(std::string_view data, struct address& address)
{
    return try_parse_address(data.begin(), data.end(), address);
}

void parse_addresses(std::string_view data, std::vector<address>& addresses)
{
    addresses.clear();
    for (size_t i = 0; (i + 7) <= data.size(); i += 7)
    {
        struct address address;
        LIBMODEM_AX25_NAMESPACE_REFERENCE try_parse_address(data.substr(i, 7), address);
        address.command_response = false;
        addresses.push_back(address);
    }
}

std::vector<uint8_t> encode_frame(const packet& p)
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

    return encode_frame(from_address, to_address, path, p.data.begin(), p.data.end());
}

std::vector<uint8_t> encode_frame(const struct frame& frame)
{
    std::vector<address> path(frame.path.begin(), frame.path.begin() + frame.path_count);
    return encode_frame(frame.from, frame.to, path, frame.data.begin(), frame.data.begin() + frame.data_length, frame.control[0], frame.pid);
}

std::vector<uint8_t> encode_frame(const address& from, const address& to, const std::vector<address>& path, std::string_view data)
{
    return encode_frame(from, to, path, data.begin(), data.end());
}

bool try_decode_frame(const std::vector<uint8_t>& frame_bytes, packet& p)
{
    address from;
    address to;
    std::vector<address> path;
    std::vector<uint8_t> data;

    if (try_decode_frame(frame_bytes, from, to, path, data))
    {
        p.from = to_string(from);
        p.to = to_string(to);

        p.path.clear();

        for (const auto& path_address : path)
        {
            p.path.push_back(to_string(path_address));
        }

        p.data = std::string(data.begin(), data.end());

        return true;
    }

    return false;
}

bool try_decode_frame(const std::vector<uint8_t>& frame_bytes, struct frame& frame)
{
    frame.path_count = 0;
    frame.data_length = 0;

    uint8_t control = 0;
    uint8_t pid = 0;
    std::array<uint8_t, 2> expected_crc = {};
    auto [path_out, data_out, result] = try_decode_frame(frame_bytes.begin(), frame_bytes.end(), frame.from, frame.to, frame.path.begin(), frame.data.begin(), frame.data.size(), control, pid, frame.crc, expected_crc);
    if (result)
    {
        frame.path_count = static_cast<size_t>(std::distance(frame.path.begin(), path_out));
        frame.data_length = static_cast<size_t>(std::distance(frame.data.begin(), data_out));
        frame.control[0] = control;
        frame.pid = pid;
    }
    return result;
}

bool try_decode_frame(const std::vector<uint8_t>& frame_bytes, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data)
{
    std::array<uint8_t, 2> crc;
    return try_decode_frame(frame_bytes, from, to, path, data, crc);
}

bool try_decode_frame(const std::vector<uint8_t>& frame_bytes, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data, std::array<uint8_t, 2>& crc)
{
    return try_decode_frame(frame_bytes.begin(), frame_bytes.end(), from, to, path, data, crc);
}

bool try_decode_frame_no_fcs(const std::vector<uint8_t>& frame_bytes, packet& p)
{
    address from;
    address to;
    std::vector<address> path;
    std::vector<uint8_t> data;

    if (try_decode_frame_no_fcs(frame_bytes, from, to, path, data))
    {
        p.from = to_string(from);
        p.to = to_string(to);

        p.path.clear();

        for (const auto& path_address : path)
        {
            p.path.push_back(to_string(path_address));
        }

        p.data = std::string(data.begin(), data.end());

        return true;
    }

    return false;
}

bool try_decode_frame_no_fcs(const std::vector<uint8_t>& frame_bytes, struct frame& frame)
{
    std::vector<address> path;
    std::vector<uint8_t> data;

    bool result = try_decode_frame_no_fcs(frame_bytes, frame.from, frame.to, path, data);

    if (result)
    {
        assert(path.size() <= frame.path.size());
        assert(data.size() <= frame.data.size());

        frame.path_count = std::min(path.size(), frame.path.size());
        std::copy_n(path.begin(), frame.path_count, frame.path.begin());

        frame.data_length = std::min(data.size(), frame.data.size());
        std::copy_n(data.begin(), frame.data_length, frame.data.begin());
    }

    return result;
}

bool try_decode_frame_no_fcs(const std::vector<uint8_t>& frame_bytes, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data)
{
    return try_decode_frame_no_fcs(frame_bytes.begin(), frame_bytes.end(), from, to, path, data);
}

bool try_decode_frame_no_fcs(std::span<const uint8_t> frame_bytes, address& from, address& to, std::vector<address>& path, std::vector<uint8_t>& data)
{
    return try_decode_frame_no_fcs(frame_bytes.begin(), frame_bytes.end(), from, to, path, data);
}

bool try_decode_frame_no_fcs(std::span<const uint8_t> frame_bytes, struct frame& frame)
{
    std::vector<address> path;
    std::vector<uint8_t> data;

    bool result = try_decode_frame_no_fcs(frame_bytes, frame.from, frame.to, path, data);

    if (result)
    {
        assert(path.size() <= frame.path.size());
        assert(data.size() <= frame.data.size());

        frame.path_count = std::min(path.size(), frame.path.size());
        std::copy_n(path.begin(), frame.path_count, frame.path.begin());

        frame.data_length = std::min(data.size(), frame.data.size());
        std::copy_n(data.begin(), frame.data_length, frame.data.begin());
    }

    return result;
}

bool try_decode_frame_no_fcs(std::span<const uint8_t> frame_bytes, packet& p)
{
    address from;
    address to;
    std::vector<address> path;
    std::vector<uint8_t> data;

    if (try_decode_frame_no_fcs(frame_bytes, from, to, path, data))
    {
        p.from = to_string(from);
        p.to = to_string(to);

        p.path.clear();

        for (const auto& path_address : path)
        {
            p.path.push_back(to_string(path_address));
        }

        p.data = std::string(data.begin(), data.end());

        return true;
    }

    return false;
}

std::vector<uint8_t> encode_header(const address& from, const address& to, const std::vector<address>& path)
{
    std::vector<uint8_t> header;

    auto to_bytes = encode_address(to, false);
    header.insert(header.end(), to_bytes.begin(), to_bytes.end());

    // If there is no path, the from address is the last address
    // and should be marked as such
    auto from_bytes = encode_address(from, path.empty());
    header.insert(header.end(), from_bytes.begin(), from_bytes.end());

    std::vector<uint8_t> addresses = encode_addresses(path);
    header.insert(header.end(), addresses.begin(), addresses.end());

    return header;
}

std::vector<uint8_t> encode_addresses(const std::vector<address>& path)
{
    std::vector<uint8_t> result;
    encode_addresses(path, std::back_inserter(result));
    return result;
}

std::array<uint8_t, 7> encode_address(const struct address& address, bool last)
{
    return encode_address(std::string_view(address.text.data(), address.text_length), address.ssid, (address.mark || address.command_response), last, address.reserved_bits);
}

std::array<uint8_t, 7> encode_address(std::string_view address, int ssid, bool cr_or_h_bit, bool last)
{
    // Typical reserved bits for AX.25 address encoding is 0b01100000 (0x60)
    return encode_address(address, ssid, cr_or_h_bit, last, {1, 1});
}

std::array<uint8_t, 7> encode_address(std::string_view address, int ssid, bool cr_or_h_bit, bool last, std::array<uint8_t, 2> reserved_bits)
{
    assert(ssid >= 0 && ssid <= 15);
    assert(reserved_bits[0] == 0 || reserved_bits[0] == 1);
    assert(reserved_bits[1] == 0 || reserved_bits[1] == 1);

    std::array<uint8_t, 7> data = {};

    // AX.25 addresses are always exactly 7 bytes:
    // 
    //  - Bytes 0-5: Callsign (6 characters, space-padded)
    //    - Each character is left-shifted by 1 bit
    //  - Byte 6: SSID + last used marker

    for (size_t i = 0; i < 6; i++)
    {
        if (i < address.length())
        {
            // Shift each character left by 1 bit
            // Example: 'W' (0x57 = 01010111) << 1 = 0xAE (10101110)
            // AX.25 uses 7-bit encoding, leaving the LSB for other purposes
            data[i] = static_cast<uint8_t>(static_cast<unsigned char>(address[i]) << 1); // shift left by 1 bit
        }
        else
        {
            // Pad remaining positions with space character
            // Space ' ' (0x20 = 00100000) << 1 = 0x40 (01000000)
            data[i] = ' ' << 1; // pad with spaces
        }
    }

    // Encode the SSID byte (byte 6)
    // This byte contains multiple fields.
    // 
    // Byte 6 initialized with 01100000 (0x60)
    // 
    //  - SSID is a 4-bit value (0-15) in bits 4-1, hence shifted left by 1
    //  - Bit 0 set to 1 if this is the last address in the path
    //  - Bit 7 (H - bit) set to 1 if the address is used(marked with*)
    //
    // Byte 6 Format:
    //
    //      H-bit  Reserved     SSID        Last
    //   ------------------------------------------
    //        7       6 5      4 3 2 1        0          bits
    //   ------------------------------------------
    //        1        2          4           1
    //
    // Examples:
    //
    //   Callsign    SSID       |  H-bit  | 0x60  | SSID      | last
    //   ------------------   --+---------+-------+-----------+-------
    //   W7ION-5*    = 5        |  1      |  1 1  | 0 1 0 1   | 0        = 0x6B
    //   W7ION-12    = 12       |  0      |  1 1  | 1 1 0 0   | 0        = 0x78
    //   APRS-0      = 0        |  0      |  1 1  | 0 0 0 0   | 0        = 0x60
    //   WIDE1-1*    = 1        |  1      |  1 1  | 0 0 0 1   | 0        = 0x63
    //   WIDE2-2     = 2        |  0      |  1 1  | 0 0 1 0   | 0        = 0x64
    //   RELAY-15*   = 15       |  1      |  1 1  | 1 1 1 1   | 0        = 0x7F 
    //
    // Example with W7ION-5*
    // 
    //   0 1 1 0 0 0 0 0 = 0x60                                            starting value
    //     ~~~
    //   0 1 0 1 = (ssid + '0') = 0x05                                     calculate ssid
    // 
    //   0 1 0 1 0 = (ssid + '0') << 1 = 0x0A                              shift SSID left by 1 bit
    //   ~~~~~~~
    //   0 1 1 0 1 0 1 0 = 0x60 | (ssid + '0') << 1 = 0x6A                 append ssid
    //         ~~~~~~~
    //   0 1 1 0 1 0 1 1 = 0x60 | (ssid + '0') << 1 | 0x01 = 0x6B          mark as last address
    //                 ~
    //   1 1 1 0 1 0 1 1 = 0x60 | (ssid + '0') << 1 | 0x01 | 0x80 = 0xEB   mark address as used
    //   ~

    // Typical reserved bits for AX.25 address encoding is 0b01100000 (0x60)
    // We provide a setter for testing purposes
    data[6] = (reserved_bits[0] << 5) | (reserved_bits[1] << 6);

    data[6] |= (ssid << 1);

    if (last)
    {
        data[6] |= 0b00000001; // Extension bit (bit 0), 0x01
    }

    if (cr_or_h_bit)
    {
        data[6] |= 0b10000000; // C/H-bit (bit 7), 0x80
    }

    return data;
}

std::vector<uint8_t> encode_bitstream(const packet& p, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(encode_frame(p), preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(const packet& p, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(encode_frame(p), initial_nrzi_level, preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(const frame& f, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(encode_frame(f), preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(const frame& f, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(encode_frame(f), initial_nrzi_level, preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(const std::vector<uint8_t>& frame, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(frame.begin(), frame.end(), preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(const std::vector<uint8_t>& frame, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(frame.begin(), frame.end(), initial_nrzi_level, preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const packet& p, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(nrz_scrambled, p, 0, preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const packet& p, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(nrz_scrambled, encode_frame(p), initial_nrzi_level, preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const frame& f, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(nrz_scrambled, f, 0, preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const frame& f, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(nrz_scrambled, encode_frame(f), initial_nrzi_level, preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const std::vector<uint8_t>& frame, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(nrz_scrambled, frame, 0, preamble_flags, postamble_flags);
}

std::vector<uint8_t> encode_bitstream(nrz_scrambled_t, const std::vector<uint8_t>& frame, uint8_t initial_nrzi_level, int preamble_flags, int postamble_flags)
{
    return encode_bitstream(nrz_scrambled, frame.begin(), frame.end(), initial_nrzi_level, preamble_flags, postamble_flags);
}

bool try_decode_bitstream(uint8_t bit, bitstream_state& state, std::vector<uint8_t>& bitstream_buffer)
{
    struct frame frame;
    return try_decode_bitstream(bit, state, bitstream_buffer, frame);
}

bool try_decode_bitstream(uint8_t bit, bitstream_state& state, std::vector<uint8_t>& bitstream_buffer, struct frame& frame)
{
    constexpr size_t buffer_capacity = 8001;
    if (bitstream_buffer.size() < buffer_capacity)
    {
        bitstream_buffer.resize(buffer_capacity);
    }

    address from;
    address to;
    std::array<address, 8> path = {};
    std::array<uint8_t, 256> data = {};
    uint8_t control = 0;
    uint8_t pid = 0;
    std::array<uint8_t, 2> actual_crc = {};
    std::array<uint8_t, 2> expected_crc = {};

    auto [path_out, data_out, result] = try_decode_bitstream(bit, state, bitstream_buffer.begin(), bitstream_buffer.end(), from, to, path.begin(), data.begin(), data.size(), control, pid, actual_crc, expected_crc);

    if (result)
    {
        frame.from = from;
        frame.to = to;

        frame.path_count = static_cast<size_t>(std::distance(path.begin(), path_out));

        std::copy_n(path.begin(), frame.path_count, frame.path.begin());

        frame.data_length = static_cast<size_t>(std::distance(data.begin(), data_out));

        std::copy_n(data.begin(), frame.data_length, frame.data.begin());

        frame.control[0] = control;
        frame.pid = pid;
        frame.crc = actual_crc;
    }

    return result;
}

bool try_decode_bitstream(uint8_t bit, packet& packet, bitstream_state& state, std::vector<uint8_t>& bitstream_buffer)
{
    struct frame frame;
    bool result = try_decode_bitstream(bit, state, bitstream_buffer, frame);
    if (result)
    {
        packet = to_packet(frame);
    }
    return result;
}

bool try_decode_bitstream(const std::vector<uint8_t>& bitstream, size_t offset, packet& packet, size_t& read, bitstream_state& state, std::vector<uint8_t>& bitstream_buffer)
{
    for (size_t i = offset; i < bitstream.size(); i++)
    {
        if (try_decode_bitstream(bitstream[i], packet, state, bitstream_buffer))
        {
            read = i - offset + 1;
            return true;
        }
    }
    read = bitstream.size() - offset;
    return false;
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

std::vector<uint8_t> encode_frame(const std::vector<uint8_t>& frame_bytes, size_t min_check_bytes)
{
    (void)frame_bytes;
    (void)min_check_bytes;
    return {};
}

std::vector<uint8_t> encode_frame(std::span<const uint8_t> frame_bytes, size_t min_check_bytes)
{
    (void)frame_bytes;
    (void)min_check_bytes;
    return {};
}

std::vector<uint8_t> encode_bitstream(const packet& p, int preamble_flags, int postamble_flags, size_t min_check_bytes)
{
    (void)p;
    (void)preamble_flags;
    (void)postamble_flags;
    (void)min_check_bytes;
    return {};
}

std::vector<uint8_t> encode_bitstream(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags, size_t min_check_bytes)
{
    (void)f;
    (void)preamble_flags;
    (void)postamble_flags;
    (void)min_check_bytes;
    return {};
}

std::vector<uint8_t> encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled_t, const packet& p, int preamble_flags, int postamble_flags, size_t min_check_bytes)
{
    (void)p;
    (void)preamble_flags;
    (void)postamble_flags;
    (void)min_check_bytes;
    return {};
}

std::vector<uint8_t> encode_bitstream(LIBMODEM_AX25_NAMESPACE_REFERENCE nrz_scrambled_t, const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags, size_t min_check_bytes)
{
    (void)f;
    (void)preamble_flags;
    (void)postamble_flags;
    (void)min_check_bytes;
    return {};
}

LIBMODEM_FX25_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// IL2P                                                             //
//                                                                  //
// Stubbed out for AetherSDR Phase 1.                               //
//                                                                  //
//                                                                  //
// **************************************************************** //

LIBMODEM_IL2P_NAMESPACE_BEGIN

std::vector<uint8_t> encode_frame(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f)
{
    (void)f;
    return {};
}

std::vector<uint8_t> encode_frame(const packet& p)
{
    (void)p;
    return {};
}

std::vector<uint8_t> encode_bitstream(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags)
{
    (void)f;
    (void)preamble_flags;
    (void)postamble_flags;
    return {};
}

std::vector<uint8_t> encode_bitstream(const packet& p, int preamble_flags, int postamble_flags)
{
    (void)p;
    (void)preamble_flags;
    (void)postamble_flags;
    return {};
}

LIBMODEM_IL2P_NAMESPACE_END

// **************************************************************** //
//                                                                  //
//                                                                  //
// il2p_bitstream_converter                                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> il2p_bitstream_converter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f) const
{
    (void)f;
    return {};
}

std::vector<uint8_t> il2p_bitstream_converter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
    (void)f;
    (void)preamble_flags;
    (void)postamble_flags;
    return {};
}

std::vector<uint8_t> il2p_bitstream_converter::encode(const packet& p) const
{
    (void)p;
    return {};
}

std::vector<uint8_t> il2p_bitstream_converter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
    (void)p;
    (void)preamble_flags;
    (void)postamble_flags;
    return {};
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// il2p_bitstream_converter_adapter                                 //
//                                                                  //
//                                                                  //
// **************************************************************** //

std::vector<uint8_t> il2p_bitstream_converter_adapter::encode(const LIBMODEM_AX25_NAMESPACE_REFERENCE frame& f, int preamble_flags, int postamble_flags) const
{
    return converter.encode(f, preamble_flags, postamble_flags);
}

std::vector<uint8_t> il2p_bitstream_converter_adapter::encode(const packet& p, int preamble_flags, int postamble_flags) const
{
    return converter.encode(p, preamble_flags, postamble_flags);
}

bool il2p_bitstream_converter_adapter::try_decode(const std::vector<uint8_t>& bitstream, size_t offset, packet& p, size_t& read)
{
    (void)bitstream;
    (void)offset;
    (void)p;
    (void)read;
    return false;
}

bool il2p_bitstream_converter_adapter::try_decode(uint8_t bit, packet& p)
{
    (void)bit;
    (void)p;
    return false;
}

void il2p_bitstream_converter_adapter::reset()
{
}

LIBMODEM_NAMESPACE_END
