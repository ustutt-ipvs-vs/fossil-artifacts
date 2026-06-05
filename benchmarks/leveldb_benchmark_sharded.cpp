#include "fossil/repository.hpp"
#include "fossil/transaction.hpp"
#include "histogram.hpp"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fossil/detail/pmap.hpp>

// Number of key/values to place in database
static int FLAGS_num = 1000000;

static int FLAGS_reads = -1;

// Number of concurrent threads to run.
static int FLAGS_threads = 1;

// Size of each value
static int FLAGS_value_size = 100;

// Arrange to generate values that shrink to this fraction of
// their original size after compression
static double FLAGS_compression_ratio = 0.5;

// common prefix length
static int FLAGS_key_prefix = 0;

static int FLAGS_compression = 0;

static int FLAGS_shards = 1;

static const char* FLAGS_benchmarks =
    "fillseq,"
    "fillrandom,"
    "readrandom";

using pm = fossil::detail::pmap<std::uint64_t, std::string>;

class Slice
{
public:
    // Create an empty slice.
    Slice() : data_(""), size_(0) {}

    // Create a slice that refers to d[0,n-1].
    Slice(const char* d, size_t n) : data_(d), size_(n) {}

    // Create a slice that refers to the contents of "s"
    Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}

    // Create a slice that refers to s[0,strlen(s)-1]
    Slice(const char* s) : data_(s), size_(strlen(s)) {}

    // Intentionally copyable.
    Slice(const Slice&) = default;
    Slice& operator=(const Slice&) = default;

    // Return a pointer to the beginning of the referenced data
    const char* data() const { return data_; }

    // Return the length (in bytes) of the referenced data
    size_t size() const { return size_; }

    // Return true iff the length of the referenced data is zero
    bool empty() const { return size_ == 0; }

    const char* begin() const { return data(); }
    const char* end() const { return data() + size(); }

    // Return the ith byte in the referenced data.
    // REQUIRES: n < size()
    char operator[](size_t n) const
    {
        assert(n < size());
        return data_[n];
    }

    // Change this slice to refer to an empty array
    void clear()
    {
        data_ = "";
        size_ = 0;
    }

    // Drop the first "n" bytes from this slice.
    void remove_prefix(size_t n)
    {
        assert(n <= size());
        data_ += n;
        size_ -= n;
    }

    // Return a string that contains the copy of the referenced data.
    std::string ToString() const { return std::string(data_, size_); }

    // Three-way comparison.  Returns value:
    //   <  0 iff "*this" <  "b",
    //   == 0 iff "*this" == "b",
    //   >  0 iff "*this" >  "b"
    int compare(const Slice& b) const;

    // Return true iff "x" is a prefix of "*this"
    bool starts_with(const Slice& x) const
    {
        return ((size_ >= x.size_) && (memcmp(data_, x.data_, x.size_) == 0));
    }

private:
    const char* data_;
    size_t size_;
};

inline bool operator==(const Slice& x, const Slice& y)
{
    return ((x.size() == y.size()) && (memcmp(x.data(), y.data(), x.size()) == 0));
}

inline bool operator!=(const Slice& x, const Slice& y) { return !(x == y); }

inline int Slice::compare(const Slice& b) const
{
    const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
    int r = memcmp(data_, b.data_, min_len);
    if(r == 0) {
        if(size_ < b.size_)
            r = -1;
        else if(size_ > b.size_)
            r = +1;
    }
    return r;
}

static void AppendWithSpace(std::string* str, Slice msg)
{
    if(msg.empty())
        return;
    if(!str->empty()) {
        str->push_back(' ');
    }
    str->append(msg.data(), msg.size());
}

class Random
{
private:
    uint32_t seed_;

public:
    explicit Random(uint32_t s) : seed_(s & 0x7fffffffu)
    {
        // Avoid bad seeds.
        if(seed_ == 0 || seed_ == 2147483647L) {
            seed_ = 1;
        }
    }
    uint32_t Next()
    {
        static const uint32_t M = 2147483647L; // 2^31-1
        static const uint64_t A = 16807; // bits 14, 8, 7, 5, 2, 1, 0
        // We are computing
        //       seed_ = (seed_ * A) % M,    where M = 2^31-1
        //
        // seed_ must not be zero or M, or else all subsequent computed values
        // will be zero or M respectively.  For all other values, seed_ will end
        // up cycling through every number in [1,M-1]
        uint64_t product = seed_ * A;

        // Compute (product % M) using the fact that ((x << 31) % M) == x.
        seed_ = static_cast<uint32_t>((product >> 31) + (product & M));
        // The first reduction may overflow by 1 bit, so we may need to
        // repeat.  mod == M is not possible; using > allows the faster
        // sign-bit-based test.
        if(seed_ > M) {
            seed_ -= M;
        }
        return seed_;
    }
    // Returns a uniformly distributed value in the range [0..n-1]
    // REQUIRES: n > 0
    uint32_t Uniform(int n) { return Next() % n; }

    // Randomly returns true ~"1/n" of the time, and false otherwise.
    // REQUIRES: n > 0
    bool OneIn(int n) { return (Next() % n) == 0; }

    // Skewed: pick "base" uniformly from range [0,max_log] and then
    // return "base" random bits.  The effect is to pick a number in the
    // range [0,2^max_log-1] with exponential bias towards smaller numbers.
    uint32_t Skewed(int max_log) { return Uniform(1 << Uniform(max_log + 1)); }
};

// forward declaration
static Slice CompressibleString(Random* rnd,
                                double compressed_fraction,
                                size_t len,
                                std::string* dst);

// Helper for quickly generating random data.
class RandomGenerator
{
private:
    std::string data_;
    int pos_;

public:
    RandomGenerator()
    {
        // We use a limited amount of data over and over again and ensure
        // that it is larger than the compression window (32KB), and also
        // large enough to serve all typical value sizes we want to write.
        Random rnd(301);
        std::string piece;
        while(data_.size() < 1048576) {
            // Add a short fragment that is as compressible as specified
            // by FLAGS_compression_ratio.
            CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
            data_.append(piece);
        }
        pos_ = 0;
    }

    Slice Generate(size_t len)
    {
        if(pos_ + len > data_.size()) {
            pos_ = 0;
            assert(len < data_.size());
        }
        pos_ += len;
        return Slice(data_.data() + pos_ - len, len);
    }
};


Slice RandomString(Random* rnd, int len, std::string* dst)
{
    dst->resize(len);
    for(int i = 0; i < len; i++) {
        (*dst)[i] = static_cast<char>(' ' + rnd->Uniform(95)); // ' ' .. '~'
    }
    return Slice(*dst);
}

std::string RandomKey(Random* rnd, int len)
{
    // Make sure to generate a wide variety of characters so we
    // test the boundary conditions for short-key optimizations.
    static const char kTestChars[] = {'\0', '\1', 'a', 'b', 'c', 'd', 'e', '\xfd', '\xfe', '\xff'};
    std::string result;
    for(int i = 0; i < len; i++) {
        result += kTestChars[rnd->Uniform(sizeof(kTestChars))];
    }
    return result;
}

static Slice CompressibleString(Random* rnd,
                                double compressed_fraction,
                                size_t len,
                                std::string* dst)
{
    int raw = static_cast<int>(len * compressed_fraction);
    if(raw < 1)
        raw = 1;
    std::string raw_data;
    RandomString(rnd, raw, &raw_data);

    // Duplicate the random data until we have filled "len" bytes
    dst->clear();
    while(dst->size() < len) {
        dst->append(raw_data);
    }
    dst->resize(len);
    return Slice(*dst);
}


class Stats
{
private:
    double start_;
    double finish_;
    double seconds_;
    int done_;
    int next_report_;
    int64_t bytes_;
    double last_op_finish_;
    fossil::bench::util::histogram hist_;
    std::string message_;

public:
    Stats() { Start(); }

    void Start()
    {
        next_report_ = 100;
        hist_.clear();
        done_ = 0;
        bytes_ = 0;
        seconds_ = 0;
        message_.clear();
        start_ = finish_ = last_op_finish_ = std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::system_clock::now()
                                                     .time_since_epoch())
                                                 .count();
    }

    void Merge(const Stats& other)
    {
        hist_.merge(other.hist_);
        done_ += other.done_;
        bytes_ += other.bytes_;
        seconds_ += other.seconds_;
        if(other.start_ < start_)
            start_ = other.start_;
        if(other.finish_ > finish_)
            finish_ = other.finish_;

        // Just keep the messages from one thread
        if(message_.empty())
            message_ = other.message_;
    }

    void Stop()
    {
        finish_ = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
        seconds_ = (finish_ - start_) * 1e-6;
    }

    void AddMessage(Slice msg) { AppendWithSpace(&message_, msg); }

    void FinishedSingleOp()
    {
        double now = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
        double micros = now - last_op_finish_;
        hist_.add(micros);
        if(micros > 20000) {
            std::fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
            std::fflush(stderr);
        }
        last_op_finish_ = now;

        done_++;
        if(done_ >= next_report_) {
            if(next_report_ < 1000)
                next_report_ += 100;
            else if(next_report_ < 5000)
                next_report_ += 500;
            else if(next_report_ < 10000)
                next_report_ += 1000;
            else if(next_report_ < 50000)
                next_report_ += 5000;
            else if(next_report_ < 100000)
                next_report_ += 10000;
            else if(next_report_ < 500000)
                next_report_ += 50000;
            else
                next_report_ += 100000;
            std::fprintf(stderr, "... finished %d ops%30s\r", done_, "");
            std::fflush(stderr);
        }
    }

    void AddBytes(int64_t n) { bytes_ += n; }

    void Report(const Slice& name)
    {
        // Pretend at least one op was done in case we are running a benchmark
        // that does not call FinishedSingleOp().
        if(done_ < 1)
            done_ = 1;

        std::string extra;
        if(bytes_ > 0) {
            // Rate is computed on actual elapsed time, not the sum of per-thread
            // elapsed times.
            double elapsed = (finish_ - start_) * 1e-6;
            char rate[100];
            std::snprintf(rate, sizeof(rate), "%6.1f MB/s", (bytes_ / 1048576.0) / elapsed);
            extra = rate;
        }
        AppendWithSpace(&extra, message_);

        std::fprintf(stdout,
                     "%-12s : %11.3f micros/op;%s%s\n",
                     name.ToString().c_str(),
                     seconds_ * 1e6 / done_,
                     (extra.empty() ? "" : " "),
                     extra.c_str());
        std::fprintf(stdout, "Microseconds per op:\n%s\n", hist_.to_string().c_str());
        std::fflush(stdout);
    }
};
class CondVar
{
public:
    explicit CondVar(std::mutex* mu) : mu_(mu) { assert(mu != nullptr); }
    ~CondVar() = default;

    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    void Wait()
    {
        std::unique_lock<std::mutex> lock(*mu_, std::adopt_lock);
        cv_.wait(lock);
        lock.release();
    }
    void Signal() { cv_.notify_one(); }
    void SignalAll() { cv_.notify_all(); }

private:
    std::condition_variable cv_;
    std::mutex* const mu_;
};

// State shared by all concurrent executions of the same benchmark.
struct SharedState
{
    std::mutex mu{};
    CondVar cv;
    int total;

    // Each thread goes through the following states:
    //    (1) initializing
    //    (2) waiting for others to be initialized
    //    (3) running
    //    (4) done

    int num_initialized;
    int num_done;
    bool start;

    SharedState(int total) : cv(&mu), total(total), num_initialized(0), num_done(0), start(false) {}
};
static Slice TrimSpace(Slice s)
{
    size_t start = 0;
    while(start < s.size() && isspace(s[start])) {
        start++;
    }
    size_t limit = s.size();
    while(limit > start && isspace(s[limit - 1])) {
        limit--;
    }
    return Slice(s.data() + start, limit - start);
}

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState
{
    int tid; // 0..n-1 when running in n threads
    Random rand; // Has different seeds for different threads
    Stats stats;
    SharedState* shared;

    ThreadState(int index, int seed) : tid(index), rand(seed), shared(nullptr) {}
};
class Benchmark
{
private:
    int num_;
    int value_size_;
    int entries_per_batch_;
    int reads_;
    int heap_counter_;
    int total_thread_count_;
    std::vector<fossil::reference<pm>> shards_;

    void PrintHeader()
    {
        const int kKeySize = 16 + FLAGS_key_prefix;
        PrintEnvironment();
        std::fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
        std::fprintf(stdout, "Shards:       %d\n", shards_.size());
        std::fprintf(stdout,
                     "Values:     %d bytes each (%d bytes after compression)\n",
                     FLAGS_value_size,
                     static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
        std::fprintf(stdout, "Entries:    %d\n", num_);
        std::fprintf(stdout,
                     "RawSize:    %.1f MB (estimated)\n",
                     ((static_cast<int64_t>(kKeySize + FLAGS_value_size) * num_) / 1048576.0));
        std::fprintf(stdout,
                     "FileSize:   %.1f MB (estimated)\n",
                     (((kKeySize + FLAGS_value_size * FLAGS_compression_ratio) * num_) /
                      1048576.0));
        PrintWarnings();
        std::fprintf(stdout, "------------------------------------------------\n");
    }

    void PrintWarnings()
    {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
        std::fprintf(stdout, "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n");
#endif
#ifndef NDEBUG
        std::fprintf(stdout, "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
    }

    void PrintEnvironment()
    {
        std::fprintf(stderr, "Fossil\n");

#if defined(__linux)
        time_t now = time(nullptr);
        std::fprintf(stderr, "Date:       %s",
                     ctime(&now)); // ctime() adds newline

        FILE* cpuinfo = std::fopen("/proc/cpuinfo", "r");
        if(cpuinfo != nullptr) {
            char line[1000];
            int num_cpus = 0;
            std::string cpu_type;
            std::string cache_size;
            while(fgets(line, sizeof(line), cpuinfo) != nullptr) {
                const char* sep = strchr(line, ':');
                if(sep == nullptr) {
                    continue;
                }
                Slice key = TrimSpace(Slice(line, sep - 1 - line));
                Slice val = TrimSpace(Slice(sep + 1));
                if(key == "model name") {
                    ++num_cpus;
                    cpu_type = val.ToString();
                } else if(key == "cache size") {
                    cache_size = val.ToString();
                }
            }
            std::fclose(cpuinfo);
            std::fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
            std::fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
        }
#endif
    }

public:
    Benchmark()
        : num_(FLAGS_num),
          value_size_(FLAGS_value_size),
          entries_per_batch_(1),
          reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
          heap_counter_(0),
          total_thread_count_(0)
    {
        shards_.reserve(FLAGS_shards);
        for(int i = 0; i < FLAGS_shards; ++i) {
            shards_.push_back(fossil::object_repo().create<pm>());
        }
    }

    ~Benchmark() = default;

    void Run()
    {
        PrintHeader();

        const char* benchmarks = FLAGS_benchmarks;
        while(benchmarks != nullptr) {
            const char* sep = strchr(benchmarks, ',');
            Slice name;
            if(sep == nullptr) {
                name = benchmarks;
                benchmarks = nullptr;
            } else {
                name = Slice(benchmarks, sep - benchmarks);
                benchmarks = sep + 1;
            }

            // Reset parameters that may be overridden below
            num_ = FLAGS_num;
            reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
            value_size_ = FLAGS_value_size;
            entries_per_batch_ = 1;

            void (Benchmark::*method)(ThreadState*, std::vector<fossil::reference<pm>>&) = nullptr;
            bool fresh_db = false;
            int num_threads = FLAGS_threads;

            if(name == Slice("fillseq")) {
                fresh_db = true;
                method = &Benchmark::WriteSeq;
            } else if(name == Slice("fillrandom")) {
                fresh_db = true;
                method = &Benchmark::WriteRandom;
            } else if(name == Slice("readrandom")) {
                method = &Benchmark::ReadRandom;
            } else {
                if(!name.empty()) { // No error message for empty name
                    std::fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
                }
            }

            RunBenchmark(num_threads, name, method);
        }
    }

private:
    struct ThreadArg
    {
        Benchmark* bm;
        SharedState* shared;
        ThreadState* thread;
        std::vector<fossil::reference<pm>>* shards;
        void (Benchmark::*method)(ThreadState*, std::vector<fossil::reference<pm>>&);
    };

    static void ThreadBody(void* v)
    {
        ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
        SharedState* shared = arg->shared;
        ThreadState* thread = arg->thread;
        {
            std::unique_lock l(shared->mu);
            shared->num_initialized++;
            if(shared->num_initialized >= shared->total) {
                shared->cv.SignalAll();
            }
            while(!shared->start) {
                shared->cv.Wait();
            }
        }

        thread->stats.Start();
        (arg->bm->*(arg->method))(thread, *arg->shards);
        thread->stats.Stop();

        {
            std::unique_lock l(shared->mu);
            shared->num_done++;
            if(shared->num_done >= shared->total) {
                shared->cv.SignalAll();
            }
        }
    }

    void RunBenchmark(int n,
                      Slice name,
                      void (Benchmark::*method)(ThreadState*, std::vector<fossil::reference<pm>>&))
    {
        SharedState shared(n);

        ThreadArg* arg = new ThreadArg[n];
        for(int i = 0; i < n; i++) {
            arg[i].bm = this;
            arg[i].method = method;
            arg[i].shared = &shared;
            ++total_thread_count_;
            // Seed the thread's random state deterministically based upon thread
            // creation across all benchmarks. This ensures that the seeds are unique
            // but reproducible when rerunning the same set of benchmarks.
            arg[i].thread = new ThreadState(i, /*seed=*/1000 + total_thread_count_);
            arg[i].thread->shared = &shared;

            arg[i].shards = &shards_;

            // start thread
            auto t = std::thread{ThreadBody, &arg[i]};
            t.detach();
        }

        shared.mu.lock();
        while(shared.num_initialized < n) {
            shared.cv.Wait();
        }

        shared.start = true;
        shared.cv.SignalAll();
        while(shared.num_done < n) {
            shared.cv.Wait();
        }
        shared.mu.unlock();

        for(int i = 1; i < n; i++) {
            arg[0].thread->stats.Merge(arg[i].thread->stats);
        }
        arg[0].thread->stats.Report(name);

        for(int i = 0; i < n; i++) {
            delete arg[i].thread;
        }
        delete[] arg;
    }

    void WriteSeq(ThreadState* thread, std::vector<fossil::reference<pm>>& shard)
    {
        DoWrite(thread, shard, true);
    }

    void WriteRandom(ThreadState* thread, std::vector<fossil::reference<pm>>& shard)
    {
        DoWrite(thread, shard, false);
    }

    void DoWrite(ThreadState* thread, std::vector<fossil::reference<pm>>& shard, bool seq)
    {
        RandomGenerator gen;
        int64_t bytes = 0;
        for(int i = 0; i < num_; ++i) {
            const pm::key_type k = seq ? i : thread->rand.Uniform(FLAGS_num);

            auto& ref = shard[k % FLAGS_shards];

            fossil::transaction(ref, [k, &gen, this](pm& map) {
                const auto value = gen.Generate(value_size_);
                map.put(k, value.ToString());
            });

            bytes += value_size_ + sizeof(pm::key_type);
            thread->stats.FinishedSingleOp();
        }
        thread->stats.AddBytes(bytes);
    }

    void ReadRandom(ThreadState* thread, std::vector<fossil::reference<pm>>& shards)
    {
        std::string value;
        int found = 0;
        for(int i = 0; i < reads_; i++) {
            const int k = thread->rand.Uniform(FLAGS_num);

            auto& ref = shards[k % FLAGS_shards];

            fossil::transaction(ref, [k, &found, &value](const pm& map) {
                if(const auto* v_ptr = map.get(k)) {
                    value = *v_ptr;
                    found++;
                }
            });

            thread->stats.FinishedSingleOp();
        }
        char msg[100];
        std::snprintf(msg, sizeof(msg), "(%d of %d found)", found, num_);
        thread->stats.AddMessage(msg);
    }
};


int main(int argc, const char* argv[])
{
    for(int i = 1; i < argc; i++) {
        double d;
        int n;
        char junk;
        if(Slice(argv[i]).starts_with("--benchmarks=")) {
            FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
        } else if(sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
            FLAGS_compression_ratio = d;
        } else if(sscanf(argv[i], "--compression=%d%c", &n, &junk) == 1 && (n == 0 || n == 1)) {
            FLAGS_compression = n;
        } else if(sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
            FLAGS_num = n;
        } else if(sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
            FLAGS_reads = n;
        } else if(sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
            FLAGS_threads = n;
        } else if(sscanf(argv[i], "--shards=%d%c", &n, &junk) == 1) {
            FLAGS_shards = n;
        } else if(sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
            FLAGS_value_size = n;
        } else {
            std::fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
            std::exit(1);
        }
    }

    Benchmark benchmark;
    benchmark.Run();
    return 0;
}
