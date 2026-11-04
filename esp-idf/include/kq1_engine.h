// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 client engine, sans IO, as pigeonhole_protocols rust/src/protocols/katzenqt1/engine:
// chats, their logs and their pending writes over the member state machine. Engine::step takes
// an Input and returns Actions to carry out; Executor carries them out over a Backend and turns
// each into the Input that reports it. File attachments and long messages (chunk streams) are
// left out: a box too large to write is refused with a notice, and a chunk stream is stepped past.
#pragma once
#include <map>
#include <set>
#include <string>

#include "kq1_member.h"

namespace kq1 {

using ChatId = std::string;

// ---- the transport's results ----

struct Keypair { Bytes write_cap, read_cap, first; };
struct Minted {
    Bytes voucher, payload, write_cap, read_cap, secret_key;
    uint8_t version = 2;  // the payload's form: 2 compact, 1 long
};
struct Inducted { std::string name; Bytes mutated_read_cap, sealed_reply; uint8_t version = 2; };
struct Opened { Bytes who_reply, mutated_write_cap; };
// The form of a voucher payload: 1 for the map form, else 2.
inline uint8_t voucher_version(const Bytes& payload) { return !payload.empty() && payload[0] >> 5 == 5 ? 1 : 2; }

struct Outcome {
    enum Kind { DATA, TOMBSTONE, NOT_FOUND } kind = NOT_FOUND;
    Bytes plaintext;
    Index next;
};

// ---- notices ----

enum class NoticeKind {
    NameTaken, NameTakenJoining, Minting, NewGroup, VoucherMinted, ReplyUnreadable, Joined, NotJoinedYet,
    AlreadyAdding, NotAVoucher, ReadingVoucher, Added, NotJoined, FileTooLarge, SendLost, FileTooLargeToFetch,
    Scanning, NoLongerMember, StoppedReading, Renamed, ActionFailed, TooLong, NotWritten, MemberAdded, Unreadable,
    NotFetched, Desync,
};
struct Notice { NoticeKind kind; std::vector<std::string> args; };
std::string plain(const Notice& n);

// ---- inputs and actions ----

struct Input {
    enum Kind {
        CREATE, JOIN, INDUCT, SEND, SCAN, LEAVE, REMOVE_MEMBER, CLEAR, RENAME, CREATED, WROTE, REFUSED, READ, WAKE,
        SCAN_READ, WALKED, DERIVED, MINTED, OPENED, JOINER_PAYLOAD, SEALED_REPLY, PUBLISHED, EPOCH, FAILED,
    } kind;
    explicit Input(Kind k) : kind(k) {}
    ChatId chat;
    std::string name, code, text, to, what;  // CREATE/JOIN name, INDUCT code, SEND text, RENAME to, FAILED what
    Key who;                                 // REMOVE_MEMBER
    bool joining = false;                    // CREATED
    Keypair keypair;                         // CREATED
    size_t job = 0;                          // WROTE, REFUSED
    Index index, next;                       // WROTE; READ, SCAN_READ: index
    Key stream;                              // READ, WAKE, SCAN_READ, WALKED, DERIVED
    Outcome outcome;                         // READ, SCAN_READ
    WalkResult walked;                       // WALKED
    std::vector<Index> indexes;              // DERIVED
    Minted minted;                           // MINTED
    Index box1;                              // MINTED, JOINER_PAYLOAD
    Opened opened;                           // OPENED
    Bytes voucher, voucher_write_cap, payload;  // JOINER_PAYLOAD
    Inducted inducted;                       // SEALED_REPLY
    size_t n = 0;                            // EPOCH
};

struct Action {
    enum Kind { CREATE, WRITE, READ, WAKE, SCAN_READ, WALK, DERIVE, MINT, AWAIT_REPLY, READ_VOUCHER, SEAL, PUBLISH } kind;
    explicit Action(Kind k) : kind(k) {}
    ChatId chat;
    bool joining = false;            // CREATE
    size_t job = 0;                  // WRITE
    Bytes write_cap;                 // WRITE, MINT, AWAIT_REPLY
    Index index;                     // WRITE, READ, SCAN_READ
    Bytes box;                       // WRITE
    Key stream;                      // READ, WAKE, SCAN_READ, WALK, DERIVE
    Bytes read_cap;                  // READ, SCAN_READ
    uint64_t ms = 0;                 // WAKE
    Index start;                     // WALK, DERIVE
    std::vector<uint64_t> targets;   // WALK
    size_t keep = 0, count = 0;      // WALK keep, DERIVE count
    std::string name;                // MINT
    uint8_t form = 2;                // MINT
    Bytes secret_key, voucher_read_cap;  // AWAIT_REPLY
    Index box1;                      // AWAIT_REPLY, PUBLISH
    uint8_t version = 2;             // AWAIT_REPLY, PUBLISH
    Bytes voucher, payload, who_reply;   // READ_VOUCHER voucher; SEAL
    Bytes voucher_write_cap, reply;  // PUBLISH
};

struct FileWrite { std::string file; Bytes bytes; };
// What a step changed, for the caller to save: chats, and log lines changed or dropped.
struct Changes {
    std::set<ChatId> chats, removed;
    std::map<ChatId, std::set<size_t>> lines, dropped;
    bool empty() const { return chats.empty() && lines.empty() && dropped.empty() && removed.empty(); }
};
struct StepResult {
    std::vector<FileWrite> files;
    Changes changes;
    std::vector<Action> actions;
};

// ---- state ----

struct Job {
    size_t id = 0;
    std::optional<size_t> token;
    std::optional<Index> index;
    Bytes box;
    std::optional<size_t> line;
};
enum class Status { Sent, Pending };
struct Line {
    size_t id = 0;
    Key sender;
    Index index;
    std::string text;
    std::optional<Notice> notice;
    Status status = Status::Sent;
    std::optional<uint64_t> idx64;
    int64_t time = 0;
};
struct Joining { std::string code; Bytes secret_key, voucher_read_cap; Index box1; uint8_t version = 2; };
struct Sealed { std::string name; Bytes mutated_read_cap, reply; uint8_t version = 2; };
struct Inducting {
    Bytes voucher;
    std::optional<Bytes> payload;
    Bytes voucher_write_cap;
    Index box1;
    bool sealing = false;
    std::optional<Sealed> sealed;
};

constexpr size_t MAX_LOG = 10000;

struct Chat {
    ChatId id;
    std::string name;
    Bytes write_cap;
    Index next_write;
    std::optional<Group> group;
    OMap<Key, Index> cursors;
    OMap<Key, std::string> names;
    std::vector<Job> jobs;
    std::vector<Line> log;
    std::optional<Joining> joining;
    std::optional<Inducting> inducting;
    size_t next_id = 0;

    size_t fresh() { return next_id++; }
    // A line of ours another member has acknowledged reading.
    bool acknowledged(const Line& l) const;
};

struct State {
    std::string me;
    std::vector<Chat> chats;
    Chat* chat(const ChatId& id);
    const Chat* chat(const ChatId& id) const;
    // Whether a deferred action still concerns something there.
    bool wanted(const Action& a) const;
};

constexpr size_t WINDOW = 8;
uint64_t backoff(size_t n);
std::string voucher_code(const Bytes& voucher);
std::string sanitize(const std::string& name);

class Engine {
public:
    State state;
    size_t max_plaintext;
    size_t epoch = 0;
    uint64_t wire_version = CURRENT_VERSION;

    Engine(State s, size_t max) : state(std::move(s)), max_plaintext(max) {}
    StepResult step(int64_t now, const Input& input);
    // After a restart from saved state: the actions that were in flight, asked again.
    StepResult resume(int64_t now);

private:
    struct Flight {
        std::set<ChatId> writing;
        std::set<std::pair<ChatId, Key>> reading;
        std::map<std::pair<ChatId, Key>, size_t> misses;
        void forget(const ChatId& chat);
    } flight_;
    struct Cx;
    void handle(StepResult& out, int64_t now, const Input& input);
    void pump(StepResult& out, int64_t now);
};

// ---- carrying actions out ----

struct Read {
    enum Kind { NOT_FOUND, TOMBSTONE, DATA } kind = NOT_FOUND;
    Bytes data;
};

// What a client needs of a transport. A nullopt or FAILED result is a failure the
// executor retries or reports; error() says what it was.
struct Backend {
    enum class Write { OK, EXISTS, FAILED };
    virtual ~Backend() = default;
    virtual std::optional<Keypair> new_keypair() = 0;
    virtual Write write_box(const Bytes& write_cap, const Index& index, const Bytes& plaintext) = 0;
    virtual std::optional<Read> read_box(const Bytes& read_cap, const Index& index) = 0;
    virtual std::optional<size_t> max_plaintext() = 0;
    virtual std::optional<Minted> voucher_mint(const Bytes& write_cap, const std::string& name, uint8_t form) = 0;
    virtual std::optional<std::pair<Bytes, Bytes>> voucher_derive_stream(const Bytes& voucher) = 0;
    virtual std::optional<Inducted> voucher_induct(const Bytes& voucher, const Bytes& payload, const Bytes& who_reply) = 0;
    virtual std::optional<Opened> voucher_open(const Bytes& secret_key, const Bytes& sealed, const Bytes& write_cap) = 0;
    virtual std::string error() const { return "failed"; }
};

constexpr uint64_t RETRY_MS = 5000;

struct Done {
    enum Kind { DONE, RETRY, LATER, NOTHING } kind = NOTHING;
    std::optional<Input> input;   // DONE, LATER
    std::optional<Action> action; // RETRY
    uint64_t ms = 0;              // RETRY, LATER
};

struct Executor {
    Backend& backend;
    std::function<void(uint64_t)> sleep;  // between attempts at a write that must land
    Done perform(const Action& a);

private:
    std::optional<Done> perform_or(const Action& a);
    Backend::Write publish(const Bytes& write_cap, const Index& index, const Bytes& b);
};

}  // namespace kq1
