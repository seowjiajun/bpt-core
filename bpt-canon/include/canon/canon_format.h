#pragma once

/// \file
/// \brief On-disk format definition for `.canon` derived event files.
///
/// `.canon` files are the *normalised* tape used by the deterministic
/// backtester. Unlike `.wslog` (raw venue WS frames), each record here is
/// a length-prefixed SBE blob — the same wire format the live system
/// sends over Aeron, minus the Aeron framing.
///
/// The schema is owned by *this* repo, not the venue. Two consequences:
///   1. Schema changes are commits, not surprise venue PRs.
///   2. Multiple producers can feed the same format: `.wslog` replays
///      (decoder output written to disk), OKX historical archives,
///      Tardis dumps, etc. The backtester reads `.canon` and can't tell
///      which producer wrote it.
///
/// File layout (little-endian, packed, no implicit padding):
///
///   FileHeader (96 bytes, see CanonFileHeader)
///   repeated:
///     RecordHeader { uint64 ts_ns; uint8 event_t; uint16 sbe_len; }
///     uint8 sbe_blob[sbe_len]
///
/// One file per (venue, UTC day, instrument-class) — same sharding rule
/// bpt-tape uses, so canon and wslog align 1:1 on disk.
///
/// Path: `{root}/canon/{venue_tag}/{YYYY-MM-DD}/{venue_tag}-{kind}.canon`
///   kind = "perp" | "spot" | "option" | "mix" (free-form, producer-chosen).

#include <cstdint>

namespace bpt::canon {

/// File magic. Always `'B','P','T','C'` (0x43545042 little-endian).
inline constexpr uint8_t kMagic[4] = {'B', 'P', 'T', 'C'};

/// Schema version. Bump when *meaning* of any field changes; readers
/// refuse newer files (loud failure). Additive event types are not a
/// meaning change, but the SBE template id bump is — bump both in lockstep
/// when SBE evolves.
inline constexpr uint16_t kSchemaVersion = 1;

/// SBE template family currently embedded. Today: bpt-protocol v1
/// (MdMarketData id=7, MdTrade id=8, MdOrderBook id=20, FundingRate id=18,
/// see messages/schema/bpt-protocol.xml). Bumped when SBE templates land
/// breaking changes — readers cross-check against the version they were
/// compiled with.
inline constexpr uint16_t kSbeTemplateFamily = 1;

/// Event-type tag. The SBE blob's template id is implied by this tag
/// (so readers don't have to peek into the SBE header to dispatch).
/// New event types are *additive* — old readers see UNKNOWN, skip, and
/// continue. Removing/repurposing a value is a kSchemaVersion bump.
enum class EventType : uint8_t {
    BBO = 0,      ///< SBE MdMarketData (top-of-book + microprice helpers)
    TRADE = 1,    ///< SBE MdTrade (aggressor side included)
    BOOK = 2,     ///< SBE MdOrderBook (L2 depth)
    FUNDING = 3,  ///< SBE FundingRate
    MARK = 4,     ///< reserved — not all SBE templates exist yet
};

/// Number of bytes the file header occupies on disk. Pinned to 96 so
/// records start 8-aligned for clean SBE memcpy on the read path.
inline constexpr uint32_t kFileHeaderBytes = 96;

#pragma pack(push, 1)

/// Fixed-size file header. Read once, validated, then ignored on the hot
/// path. All multi-byte fields little-endian. `producer_kind` and
/// `producer_sha` are null-padded ASCII (NOT null-terminated; consumers
/// must respect the field length).
struct CanonFileHeader {
    uint8_t magic[4];          ///< must equal kMagic
    uint16_t schema_version;   ///< canon's own versioning (kSchemaVersion)
    uint16_t sbe_template_id;  ///< SBE family the records use (kSbeTemplateFamily)
    uint8_t venue_id;          ///< messages::ExchangeId
    uint8_t flags;             ///< reserved, must be 0 today
    uint32_t date_utc;         ///< YYYYMMDD packed decimal (e.g. 20260520)
    char producer_kind[16];    ///< e.g. "wslog-replay", "okx-archive"
    char producer_sha[40];     ///< git sha of the producer; ascii hex
    uint8_t reserved[26];      ///< zero-filled, brings struct to 96 bytes
};
static_assert(sizeof(CanonFileHeader) == kFileHeaderBytes, "CanonFileHeader on-disk size must be exactly 96 bytes");

/// Per-record header. Written immediately before the SBE blob.
struct CanonRecordHeader {
    uint64_t ts_ns;    ///< wall-clock ns since Unix epoch
    uint8_t event_t;   ///< EventType
    uint16_t sbe_len;  ///< bytes of SBE payload that follow
};
static_assert(sizeof(CanonRecordHeader) == 11, "CanonRecordHeader on-disk size must be exactly 11 bytes");

#pragma pack(pop)

/// Convenience: pack a calendar date as YYYYMMDD.
[[nodiscard]] constexpr uint32_t pack_date(uint16_t year, uint8_t month, uint8_t day) noexcept {
    return static_cast<uint32_t>(year) * 10000U + static_cast<uint32_t>(month) * 100U + static_cast<uint32_t>(day);
}

}  // namespace bpt::canon
