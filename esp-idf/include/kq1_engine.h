// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 client engine, sans IO, as pigeonhole_protocols rust/src/protocols/katzenqt1/engine:
// chats, their logs and their pending writes over the member state machine. Engine::step takes
// an Input and returns Actions to carry out; Executor carries them out over a Backend and turns
// each into the Input that reports it. A message too long for a box, a file's most often, goes
// to a substream of its own, which an I box in the member's stream names.
#pragma once
#include <map>
#include <set>
#include <string>

#include "kq1_member.h"

// Streams, bursts sent as they are made, are built only where something sends them: voice images,
// the Linux image's kq stream, and the host tests. The default T-Deck image goes without.
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
#if !defined(ESP_PLATFORM) || CONFIG_STRADDLE_LXST || CONFIG_IDF_TARGET_LINUX
#define KQ_STREAMS 1
#endif

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
    NotFetched, Desync, Pinged, Unsupported,
};
struct Notice { NoticeKind kind; std::vector<std::string> args; };
std::string plain(const Notice& n);
std::string size_text(size_t n);

// ---- inputs and actions ----

struct Input {
    enum Kind {
        CREATE, JOIN, INDUCT, SEND, SCAN, LEAVE, REMOVE_MEMBER, CLEAR, RENAME, CREATED, WROTE, REFUSED, READ, WAKE,
        SCAN_READ, WALKED, DERIVED, MINTED, OPENED, JOINER_PAYLOAD, SEALED_REPLY, PUBLISHED, EPOCH, FAILED,
        SEND_FILE, LOADED, SUBSTREAM, WROTE_CHUNK, CHUNK_INDEXES, READ_CHUNK,
        // A file sent as it is made, a radio burst while the talk key is held: started with its
        // name, its bytes added as they come, ended. Its chunks are written as they fill.
        STREAM_START, STREAM_DATA, STREAM_END,
        PING,  // a ping to the chat: its radio listeners play their ping sound
    } kind;
    explicit Input(Kind k) : kind(k) {}
    ChatId chat;
    std::string name, code, text, to, what;  // CREATE/JOIN/SEND_FILE name, INDUCT code, SEND text, RENAME to, FAILED what
    Key who;                                 // REMOVE_MEMBER
    bool joining = false;                    // CREATED
    Keypair keypair;                         // CREATED, SUBSTREAM
    size_t job = 0;                          // WROTE, REFUSED, LOADED, SUBSTREAM, WROTE_CHUNK, CHUNK_INDEXES
    Index index, next;                       // WROTE, WROTE_CHUNK next; READ, SCAN_READ, READ_CHUNK index; CHUNK_INDEXES index: start
    Key stream;                              // READ, WAKE, SCAN_READ, WALKED, DERIVED, READ_CHUNK
    Outcome outcome;                         // READ, SCAN_READ, READ_CHUNK
    WalkResult walked;                       // WALKED
    std::vector<Index> indexes;              // DERIVED, CHUNK_INDEXES
    std::optional<Bytes> bytes;              // SEND_FILE, STREAM_DATA; LOADED, nothing when the file is gone
    Minted minted;                           // MINTED
    Index box1;                              // MINTED, JOINER_PAYLOAD
    Opened opened;                           // OPENED
    Bytes voucher, voucher_write_cap, payload;  // JOINER_PAYLOAD
    Inducted inducted;                       // SEALED_REPLY
    size_t n = 0;                            // EPOCH; WROTE_CHUNK the box
};

struct Action {
    enum Kind {
        CREATE, WRITE, READ, WAKE, SCAN_READ, WALK, DERIVE, MINT, AWAIT_REPLY, READ_VOUCHER, SEAL, PUBLISH,
        DELETE, LOAD, SUBSTREAM, WRITE_CHUNK, CHUNK_INDEXES, READ_CHUNK,
    } kind;
    explicit Action(Kind k) : kind(k) {}
    ChatId chat;
    bool joining = false;            // CREATE
    size_t job = 0;                  // WRITE, LOAD, SUBSTREAM, WRITE_CHUNK, CHUNK_INDEXES
    size_t n = 0;                    // WRITE_CHUNK: the box
    Bytes write_cap;                 // WRITE, MINT, AWAIT_REPLY, WRITE_CHUNK
    Index index;                     // WRITE, READ, SCAN_READ, WRITE_CHUNK, READ_CHUNK
    Bytes box;                       // WRITE, WRITE_CHUNK
    Key stream;                      // READ, WAKE, SCAN_READ, WALK, DERIVE, READ_CHUNK
    Bytes read_cap;                  // READ, SCAN_READ, READ_CHUNK
    uint64_t ms = 0;                 // WAKE
    Index start;                     // WALK, DERIVE, CHUNK_INDEXES
    std::vector<uint64_t> targets;   // WALK
    size_t keep = 0, count = 0;      // WALK keep, DERIVE and CHUNK_INDEXES count
    std::string file;                // DELETE, LOAD
    std::string name;                // MINT
    uint8_t form = 2;                // MINT
    Bytes secret_key, voucher_read_cap;  // AWAIT_REPLY
    Index box1;                      // AWAIT_REPLY, PUBLISH
    uint8_t version = 2;             // AWAIT_REPLY, PUBLISH
    Bytes voucher, payload, who_reply;   // READ_VOUCHER voucher; SEAL
    Bytes voucher_write_cap, reply;  // PUBLISH
};

struct FileWrite { std::string file; Bytes bytes; };
// A voice burst heard in a radio group: held in memory to be played, never kept.
struct Clip { ChatId chat; Key sender; std::string who, name; Bytes bytes; };  // who: ~name, as notices say
// A ping heard in a radio group.
struct Ping { ChatId chat; Key sender; std::string who; };
// What a step changed, for the caller to save: chats, and log lines changed or dropped.
struct Changes {
    std::set<ChatId> chats, removed;
    std::map<ChatId, std::set<size_t>> lines, dropped;
    bool empty() const { return chats.empty() && lines.empty() && dropped.empty() && removed.empty(); }
};
struct StepResult {
    std::vector<FileWrite> files;
    std::vector<Clip> clips;
    std::vector<Ping> pings;
    Changes changes;
    std::vector<Action> actions;
};

// ---- state ----

// A message written to a substream: its boxes from sent on are still to write, at next on.
struct Upload {
    std::string file;  // the message, kept until its boxes are written; none for a stream
    size_t box_size = 0, total = 0;  // total 0: a stream not yet ended
    Bytes write_cap;   // the substream's; empty until made
    size_t sent = 0;
    Index next;
    bool operator==(const Upload& o) const {
        return file == o.file && box_size == o.box_size && total == o.total && write_cap == o.write_cap && sent == o.sent &&
               next == o.next;
    }
};
struct Job {
    size_t id = 0;
    std::optional<size_t> token;
    std::optional<Index> index;
    Bytes box;  // an upload's I box, once its substream is written
    std::optional<size_t> line;
    std::optional<Upload> upload;
};
struct Attachment {
    std::string name, filetype;
    size_t size = 0;
    std::string file;  // where its bytes are kept
};
// A member's long message being read from its substream; index is where its I box is.
struct Download {
    Key stream;
    Index index, next;
    Bytes read_cap;
    std::optional<size_t> total;
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
    std::optional<Attachment> attachment;
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
    std::vector<Download> downloads;

    size_t fresh() { return next_id++; }
    Job* job(size_t id);
    // The files its lines and uploads keep.
    std::vector<std::string> files() const;
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
// How often a radio group's members are polled: a burst is heard about this long after it lands.
constexpr uint64_t RADIO_POLL_MS = 1000;
std::string voucher_code(const Bytes& voucher);
// The voucher a code holds (whitespace skipped), if it is one.
std::optional<Bytes> from_code(const std::string& code);
// Four digits of SHAKE256(voucher), shown on both screens when a voucher goes across by sound:
// the same digits on both say the right one came. "" when code is not a voucher code.
std::string voucher_check(const std::string& code);
std::string sanitize(const std::string& name);

class Engine {
public:
    State state;
    size_t max_plaintext;
    size_t epoch = 0;
    uint64_t wire_version = CURRENT_VERSION;
    // The largest file sent, kept or fetched: a device has less room than MAX_FILE.
    size_t max_file = MAX_FILE;
    // Groups listened to as radio: their streams are polled every RADIO_POLL_MS, not backed off,
    // and the audio/opus files others send there are clips, not lines or files kept.
    std::set<ChatId> radio;

    Engine(State s, size_t max) : state(std::move(s)), max_plaintext(max) {}
    StepResult step(int64_t now, const Input& input);
    // After a restart from saved state: the actions that were in flight, asked again.
    StepResult resume(int64_t now);

private:
    using JobKey = std::pair<ChatId, size_t>;
    using StreamKey = std::pair<ChatId, Key>;
    // What is under way on the network; not saved, since a restart begins it again.
    struct Flight {
        std::set<ChatId> writing;
        std::set<StreamKey> reading;
        std::map<StreamKey, size_t> misses;
        std::set<JobKey> uploading;                              // a substream being made or indexes derived
        std::map<JobKey, std::vector<Index>> ahead;              // the index of each box from the first unwritten on
        std::map<JobKey, std::set<size_t>> chunks_out;           // the boxes being written
        std::map<JobKey, std::vector<std::pair<size_t, Index>>> landed;  // written after one not yet written
        std::set<JobKey> loading;                                // a message being read back from its file
        std::map<JobKey, Bytes> blobs;                           // each upload's message
        std::map<StreamKey, std::pair<Index, std::vector<Bytes>>> fetched;  // each download's next index and boxes
        std::map<std::pair<Key, Index>, Bytes> arriving;         // a file's bytes, while its message is read
        // Each stream being sent: its message so far (the payload's length header left at 0),
        // and what follows the payload, from the message composed at its start.
        struct Stream { Bytes data, tail; size_t header = 0; };
        std::map<JobKey, Stream> streams;
        std::map<std::pair<ChatId, std::string>, size_t> stream_jobs;  // by its name
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

// The files the engine keeps, for its DELETE and LOAD.
struct Files {
    std::function<std::optional<Bytes>(const std::string&)> read;
    std::function<void(const std::string&)> remove;
};

struct Executor {
    Backend& backend;
    std::function<void(uint64_t)> sleep;  // between attempts at a write that must land
    Files files = {};
    Done perform(const Action& a);

private:
    std::optional<Done> perform_or(const Action& a);
    Backend::Write publish(const Bytes& write_cap, const Index& index, const Bytes& b);
};

}  // namespace kq1
