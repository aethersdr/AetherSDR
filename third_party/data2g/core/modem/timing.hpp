// data2g/modem.py's burst timing: header layout, frames on air, burst
// length. Pure arithmetic on config; the rest of modem.py builds on it.
#pragma once

#include <optional>
#include <string_view>
#include <vector>
#include <cstdint>

#include "config/config.hpp"

namespace data2g::modem {

bool hosts(std::string_view band);                          // _hosts: other bands' frames follow its header
std::vector<bool> header_layout(std::string_view band);     // per header symbol: true = pilot
int header_samples(std::string_view band);
bool copies(std::string_view band);  // the band repeats its header after the first frames
std::optional<int> copy_frame(std::string_view band, int n_f);  // nullopt: no header copy
int frames_on_air(const config::Submode& spec, int n_cw);
std::int64_t burst_end(std::int64_t p0, const config::Submode& spec, int n_cw);
int head_samples(std::string_view band);
int receive_head(std::string_view band);  // receive(head=...)'s audio from a found preamble's first sample
double burst_seconds(const config::Submode& spec, int n_cw);

}  // namespace data2g::modem
