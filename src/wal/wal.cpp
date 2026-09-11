#include <wal/wal.h>
#include <msgpack.hpp>
#include <unistd.h>      // fsync, rmdir
#include <sys/stat.h>    // mkdir
#include <dirent.h>      // opendir, readdir
#include <algorithm>
#include <cstring>
#include <iostream>
#include <inttypes.h>
#include <fcntl.h>
#include <stdexcept>

namespace kv {
namespace wal {

// ---------------------------------------------------------------------------
// CRC32 (polynomial 0xEDB88320, standard table-driven implementation)
// ---------------------------------------------------------------------------
static const uint32_t kCrc32Table[256] = {
  0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
  0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
  0x0EDB8832, 0x79DC18A4, 0xE0D5E91E, 0x97D2D988,
  0x09B64C2B, 0x7EB17CBD, 0xE7B82D09, 0x90BF1D3F,
  0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
  0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
  0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
  0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
  0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
  0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
  0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
  0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
  0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
  0x21B4F6B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
  0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
  0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
  0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
  0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
  0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
  0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
  0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
  0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
  0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
  0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
  0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
  0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
  0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
  0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7AC9,
  0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
  0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
  0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
  0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
  0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
  0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
  0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
  0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
  0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
  0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
  0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
  0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
  0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
  0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
  0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
  0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
  0xCB61B38B, 0xBC66831D, 0x256FD2A7, 0x5268E231,
  0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
  0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
  0xC2D766A8, 0xB5D0569E, 0x2CD99E8B, 0x5BDEAE1D,
  0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
  0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
  0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
  0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
  0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FAAAB6E,
  0x81EE3ECD, 0xF6E90E5B, 0x6FE05FE1, 0x18E76F77,
  0x88085AE6, 0xFF0F6B70, 0x66063BCA, 0x11010B5C,
  0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
  0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
  0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
  0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
  0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
  0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
  0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
  0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
  0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

uint32_t WAL::crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = kCrc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

// ---------------------------------------------------------------------------
// Filename helpers
// ---------------------------------------------------------------------------
std::string WAL::make_wal_name(uint64_t seq, uint64_t index) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%016" PRIx64 "-%016" PRIx64 ".wal", seq, index);
  return buf;
}

bool WAL::parse_wal_name(const std::string& name, uint64_t& seq, uint64_t& index) {
  // Expect: <32 hex chars separated by '-'>.wal  →  "0000000000000000-0000000000000000.wal"
  if (name.size() < 37 || name.substr(name.size() - 4) != ".wal") {
    return false;
  }
  std::string stem = name.substr(0, name.size() - 4);  // strip ".wal"
  size_t dash = stem.find('-');
  if (dash == std::string::npos) {
    return false;
  }
  try {
    seq   = std::stoull(stem.substr(0, dash), nullptr, 16);
    index = std::stoull(stem.substr(dash + 1), nullptr, 16);
  } catch (...) {
    return false;
  }
  return true;
}

std::vector<std::string> WAL::list_wal_files(const std::string& dir) {
  std::vector<std::string> names;
  DIR* d = opendir(dir.c_str());
  if (!d) return names;

  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string name(ent->d_name);
    if (name.size() >= 4 && name.substr(name.size() - 4) == ".wal") {
      names.push_back(name);
    }
  }
  closedir(d);

  std::sort(names.begin(), names.end());
  return names;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
WAL::WAL(const std::string& dir, FILE* fp) : dir_(dir), fp_(fp) {}

WAL::~WAL() {
  if (fp_) {
    fclose(fp_);
    fp_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Factory: create (new WAL directory + initial file)
// ---------------------------------------------------------------------------
std::unique_ptr<WAL> WAL::create(const std::string& dir) {
  // mkdir -p (ok if already exists)
  if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    std::cerr << "[WAL] mkdir failed: " << dir << " (" << strerror(errno) << ")" << std::endl;
    return nullptr;
  }

  std::string path = dir + "/" + make_wal_name(0, 0);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    std::cerr << "[WAL] create failed: " << path << " (" << strerror(errno)
              << ")" << std::endl;
    return nullptr;
  }
  FILE* fp = fdopen(fd, "wb");
  if (!fp) {
    const int saved_errno = errno;
    close(fd);
    unlink(path.c_str());
    errno = saved_errno;
    std::cerr << "[WAL] fdopen failed: " << path << " (" << strerror(errno)
              << ")" << std::endl;
    return nullptr;
  }

  return std::unique_ptr<WAL>(new WAL(dir, fp));
}

// ---------------------------------------------------------------------------
// Factory: open (existing WAL directory, resume appending to latest file)
// ---------------------------------------------------------------------------
std::unique_ptr<WAL> WAL::open(const std::string& dir) {
  auto names = list_wal_files(dir);
  if (names.empty()) {
    std::cerr << "[WAL] no WAL files found in: " << dir << std::endl;
    return nullptr;
  }

  // Open the latest file for appending
  const std::string& latest = names.back();
  uint64_t seq, index;
  if (!parse_wal_name(latest, seq, index)) {
    std::cerr << "[WAL] invalid WAL filename: " << latest << std::endl;
    return nullptr;
  }

  std::string path = dir + "/" + latest;
  FILE* fp = fopen(path.c_str(), "ab");  // append-binary
  if (!fp) {
    std::cerr << "[WAL] fopen failed: " << path << " (" << strerror(errno) << ")" << std::endl;
    return nullptr;
  }

  return std::unique_ptr<WAL>(new WAL(dir, fp));
}

// ---------------------------------------------------------------------------
// Write path: append_record → buffer; sync → fwrite + fsync
// ---------------------------------------------------------------------------
void WAL::append_record(RecordType type, const uint8_t* data, size_t len) {
  constexpr std::size_t kMaxRecordPayload = (1U << 24U) - 1U;
  if (len > kMaxRecordPayload) {
    throw std::length_error("WAL record exceeds the 24-bit payload limit");
  }
  uint32_t checksum = crc32(data, len);
  RecordHeader hdr(static_cast<uint8_t>(type), static_cast<uint32_t>(len), checksum);
  hdr.encode(buffer_);                                  // 8 bytes header
  buffer_.insert(buffer_.end(), data, data + len);      // payload
}

void WAL::save_hard_state(const HardStateProto& hs) {
  if (hs.is_empty()) return;

  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, hs);
  append_record(RecordType::HardState,
                reinterpret_cast<const uint8_t*>(sbuf.data()),
                sbuf.size());
}

void WAL::save_entry(const proto::Entry& entry) {
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, entry);
  append_record(RecordType::Entry,
                reinterpret_cast<const uint8_t*>(sbuf.data()),
                sbuf.size());
}

void WAL::save_snapshot(const SnapshotMeta& snap) {
  if (snap.is_empty()) return;

  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, snap);
  append_record(RecordType::Snapshot,
                reinterpret_cast<const uint8_t*>(sbuf.data()),
                sbuf.size());
}

bool WAL::sync() {
  if (failed_) return false;
  if (buffer_.empty()) return true;

  size_t written = fwrite(buffer_.data(), 1, buffer_.size(), fp_);
  if (written != buffer_.size()) {
    std::cerr << "[WAL] fwrite short write: " << written << "/" << buffer_.size()
              << " (" << strerror(errno) << ")" << std::endl;
    failed_ = true;
    buffer_.clear();
    return false;
  }

  // fwrite may only reach stdio's userspace buffer. Flush that buffer before
  // asking the kernel to make the file durable; otherwise fsync can succeed
  // while an abrupt process exit still loses the acknowledged record.
  if (fflush(fp_) != 0) {
    std::cerr << "[WAL] fflush failed (" << strerror(errno) << ")"
              << std::endl;
    failed_ = true;
    buffer_.clear();
    return false;
  }

  if (fsync(fileno(fp_)) != 0) {
    std::cerr << "[WAL] fsync failed (" << strerror(errno) << ")" << std::endl;
    failed_ = true;
    buffer_.clear();
    return false;
  }
  buffer_.clear();
  return true;
}

// ---------------------------------------------------------------------------
// Recovery: read all WAL files sequentially, validate CRC, stop at corruption
// ---------------------------------------------------------------------------
HardStateProto WAL::recover(std::vector<proto::Entry>& entries, SnapshotMeta* snapshot) {
  HardStateProto hs;
  auto names = list_wal_files(dir_);
  if (names.empty()) {
    throw std::runtime_error("no WAL files available for recovery in " + dir_);
  }
  bool stop_recovery = false;

  for (const std::string& name : names) {
    if (stop_recovery) {
      break;
    }
    std::string path = dir_ + "/" + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
      const int open_errno = errno;
      throw std::runtime_error("cannot open WAL file " + path + ": " +
                               strerror(open_errno));
    }

    // Read entire file into memory
    std::vector<uint8_t> data;
    uint8_t chunk[4096];
    while (true) {
      size_t n = fread(chunk, 1, sizeof(chunk), f);
      data.insert(data.end(), chunk, chunk + n);
      if (n < sizeof(chunk)) {
        if (ferror(f)) {
          const int read_errno = errno;
          fclose(f);
          throw std::runtime_error("cannot read WAL file " + path + ": " +
                                   strerror(read_errno));
        }
        break;
      }
    }
    fclose(f);

    // Parse records sequentially
    size_t offset = 0;
    size_t valid_bytes = 0;
    bool damaged_tail = false;
    while (offset < data.size()) {
      const size_t record_start = offset;
      // Need at least a full header
      if (data.size() - offset < 8) {
        damaged_tail = true;
        break;  // truncated header → stop (crash during header write)
      }

      RecordHeader hdr = RecordHeader::decode(data.data() + offset);
      offset += 8;

      const bool supported_type =
          hdr.type == RecordType::Entry || hdr.type == RecordType::HardState ||
          hdr.type == RecordType::Snapshot;
      if (!supported_type) {
        damaged_tail = true;
        break;
      }

      // Not enough data for the payload → stop (crash during payload write)
      if (data.size() - offset < hdr.len) {
        damaged_tail = true;
        break;
      }

      // Validate CRC
      uint32_t computed = crc32(data.data() + offset, hdr.len);
      if (computed != hdr.crc) {
        damaged_tail = true;
        break;  // CRC mismatch → corrupted record, stop here
      }

      // Deserialize based on record type
      const uint8_t* payload = data.data() + offset;
      offset += hdr.len;

      try {
        switch (static_cast<RecordType>(hdr.type)) {
          case RecordType::HardState: {
            msgpack::object_handle oh = msgpack::unpack(
                reinterpret_cast<const char*>(payload), hdr.len);
            oh.get().convert(hs);
            break;
          }
          case RecordType::Entry: {
            proto::Entry entry;
            msgpack::object_handle oh = msgpack::unpack(
                reinterpret_cast<const char*>(payload), hdr.len);
            oh.get().convert(entry);

            auto replacement = std::lower_bound(
                entries.begin(), entries.end(), entry.index,
                [](const proto::Entry &existing, uint64_t index) {
                  return existing.index < index;
                });
            if (replacement != entries.end()) {
              entries.erase(replacement, entries.end());
            }
            entries.push_back(std::move(entry));
            break;
          }
          case RecordType::Snapshot: {
            SnapshotMeta recovered_snapshot;
            msgpack::object_handle oh = msgpack::unpack(
                reinterpret_cast<const char *>(payload), hdr.len);
            oh.get().convert(recovered_snapshot);

            if (recovered_snapshot.discard_suffix) {
              entries.clear();
            } else {
              entries.erase(
                  entries.begin(),
                  std::upper_bound(
                      entries.begin(), entries.end(), recovered_snapshot.index,
                      [](uint64_t index, const proto::Entry &entry) {
                        return index < entry.index;
                      }));
            }
            if (snapshot) {
              *snapshot = std::move(recovered_snapshot);
            }
            break;
          }
          default:
            break;
        }
      } catch (const std::exception &error) {
        std::cerr << "[WAL] recover: invalid record at byte " << record_start
                  << " in " << path << ": " << error.what() << std::endl;
        damaged_tail = true;
        break;
      }

      valid_bytes = offset;
    }

    if (damaged_tail) {
      const int repair_fd = ::open(path.c_str(), O_WRONLY);
      bool repaired = repair_fd >= 0;
      int repair_errno = repair_fd < 0 ? errno : 0;
      if (repaired &&
          ftruncate(repair_fd, static_cast<off_t>(valid_bytes)) != 0) {
        repaired = false;
        repair_errno = errno;
      }
      if (repaired && fsync(repair_fd) != 0) {
        repaired = false;
        repair_errno = errno;
      }
      if (repair_fd >= 0) {
        close(repair_fd);
      }
      if (!repaired) {
        throw std::runtime_error(
            "failed to repair damaged WAL tail in " + path + ": " +
            strerror(repair_errno));
      }
      stop_recovery = true;
    }
  }

  return hs;
}

} // namespace wal
} // namespace kv
