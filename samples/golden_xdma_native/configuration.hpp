#pragma once
#include "../golden_xdma/codec.hpp"
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
namespace golden::xdma::native {
struct Configuration {
  std::string tuple, bitstream;
  std::array<std::string, 7> paths{}; // H2C0/1, C2H0/1, user, event0/1
  std::array<std::uint64_t, 2> input{}, output{};
  bool valid() const noexcept {
    if (tuple.empty() || tuple.size() > 128 || bitstream.size() != 64 ||
        bitstream.find_first_not_of("0123456789abcdef") != std::string::npos ||
        bitstream.find_first_not_of('0') == std::string::npos)
      return false;
    for (std::size_t i = 0; i < paths.size(); ++i) {
      if (paths[i].empty() || paths[i][0] != '/' || paths[i].size() >= 4096 ||
          paths[i].find('\0') != std::string::npos)
        return false;
      for (std::size_t j = 0; j < i; ++j)
        if (paths[i] == paths[j]) return false;
    }
    const std::array<std::uint64_t, 4> offsets{input[0],output[0],input[1],output[1]};
    for (std::size_t i = 0; i < offsets.size(); ++i) {
      const auto bytes = frame_size(i);
      // Linux pread/pwrite uses signed off_t; keep all windows disjoint even
      // when both channel nodes expose the same AXI-MM address space.
      if (offsets[i] % 64 || offsets[i] > INT64_MAX - bytes) return false;
      for (std::size_t j = 0; j < i; ++j)
        if (offsets[i] < offsets[j] + frame_size(j) &&
            offsets[j] < offsets[i] + bytes) return false;
    }
    return true;
  }
};
inline bool read_configuration(const std::filesystem::path &path, Configuration &out) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec ||
      std::filesystem::file_size(path, ec) > 32768 || ec) return false;
  std::ifstream stream(path);
  std::map<std::string,std::string> rows;
  std::string line;
  while (std::getline(stream,line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto at = line.find('=');
    if (at == std::string::npos || at == 0 || at + 1 == line.size() ||
        !rows.emplace(line.substr(0,at),line.substr(at+1)).second) return false;
  }
  if (!stream.eof() || rows.size() != 15 ||
      rows["layout"] != "golden-xdma-native-v1" ||
      rows["driver_revision"] != "8721136e74a66500b02d16cb41922d966139cd46") return false;
  Configuration candidate;
  candidate.tuple = rows["tuple"]; candidate.bitstream = rows["bitstream_sha256"];
  constexpr std::array<std::string_view,7> names{"h2c0","h2c1","c2h0","c2h1","user","event0","event1"};
  for (std::size_t i=0;i<names.size();++i) candidate.paths[i]=rows[std::string(names[i])];
  constexpr std::array<std::string_view,4> offsets{"input0","output0","input1","output1"};
  std::array<std::uint64_t,4> parsed{};
  for (std::size_t i=0;i<offsets.size();++i) {
    const auto &text=rows[std::string(offsets[i])];
    const auto p=std::from_chars(text.data(),text.data()+text.size(),parsed[i]);
    if (p.ec!=std::errc{} || p.ptr!=text.data()+text.size()) return false;
  }
  if (rows.size()!=15) return false; // unknown/missing keys never default
  candidate.input={parsed[0],parsed[2]};candidate.output={parsed[1],parsed[3]};
  if (!candidate.valid()) return false;
  out=std::move(candidate);return true;
}
enum class Availability { ready, absent, invalid };
inline Availability availability(const Configuration &config) {
  if (!config.valid()) return Availability::invalid;
  bool missing=false;
  for (const auto &path:config.paths) {
    std::error_code ec;
    const auto status=std::filesystem::status(path,ec);
    if (status.type()==std::filesystem::file_type::not_found) { missing=true;continue; }
    if (ec || !std::filesystem::is_character_file(status)) return Availability::invalid;
  }
  return missing ? Availability::absent : Availability::ready;
}
} // namespace golden::xdma::native
