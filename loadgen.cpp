// Command format: ./loadgen_new <ip> <port> <clients> <duration> <workload>

#include "httplib.h"
#include "json.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;
using namespace std::chrono;

// Shared Counters
static std::atomic<bool> stop_flag{false};
static std::atomic<uint64_t> global_seq{0};
static std::atomic<uint64_t> total_success{0};
static std::atomic<uint64_t> total_errors{0};
static std::atomic<uint64_t> total_requests{0};
static std::atomic<uint64_t> total_latency_ns{0};

// CTRL+C
void signal_handler(int){ stop_flag.store(true);} 

struct WorkArgs{
    int tid;
    httplib::Client* cli;
    steady_clock::time_point end;    // end time
    uint64_t keyspace;
    uint64_t popular_size;
    int retries;
};

// Helper: key generation
static std::string key_thread(int tid, uint64_t seq, uint64_t ks){
    uint64_t v = (uint64_t)tid * 1000003ULL + seq;
    v %= ks;
    return "t"+std::to_string(tid)+"-k"+std::to_string(v);
}
static std::string key_global(){ return "g"+std::to_string(global_seq.fetch_add(1)); }

// latency measure & exponential backoff wait
static bool attempt_op(httplib::Client* cli, const std::string& type, const std::string& path, const std::string& body, int retries, uint64_t &lat_ns){
    for(int a=0;a<=retries;a++){
        auto t0=steady_clock::now();
        httplib::Result res;
        if(type=="GET") res=cli->Get(path.c_str());
        else if(type=="PUT") res=cli->Put(path.c_str(),body,"application/json");
        else if(type=="DELETE") res=cli->Delete(path.c_str());
        auto t1=steady_clock::now(); lat_ns=duration_cast<nanoseconds>(t1-t0).count();
        if(res && res->status>=200 && res->status<300) return true;
        if(a<retries) std::this_thread::sleep_for(std::chrono::milliseconds(50*(1<<a)));
    }
    return false;
}

// Workload functions
void run_get_all(WorkArgs w){
    auto end=w.end; auto cli=w.cli;
    while(!stop_flag.load() && steady_clock::now()<end){
        std::string key=key_global();
        uint64_t lat; bool ok=attempt_op(cli,"GET","/kv/"+key,"",w.retries,lat);
        total_requests.fetch_add(1, std::memory_order_relaxed);
        if(ok){total_success.fetch_add(1, std::memory_order_relaxed); total_latency_ns.fetch_add(lat, std::memory_order_relaxed);} else total_errors.fetch_add(1, std::memory_order_relaxed);
    }
}

void run_put_all(WorkArgs w){
    int tid=w.tid; auto end=w.end; auto cli=w.cli; uint64_t seq=0;
    while(!stop_flag.load() && steady_clock::now()<end){
        std::string key=key_thread(tid,seq++,w.keyspace);
        std::string type=(seq%2?"PUT":"DELETE");
        json j; j["value"]="v"+std::to_string(seq);
        uint64_t lat; bool ok=attempt_op(cli,type,"/kv/"+key,j.dump(),w.retries,lat);
        total_requests.fetch_add(1, std::memory_order_relaxed);
        if(ok){total_success.fetch_add(1, std::memory_order_relaxed); total_latency_ns.fetch_add(lat, std::memory_order_relaxed);} else total_errors.fetch_add(1, std::memory_order_relaxed);
    }
}

void run_get_popular(WorkArgs w){
    int tid=w.tid; auto end=w.end; auto cli=w.cli;
    std::vector<std::string> keys;
    for(uint64_t i=0;i<w.popular_size;i++) keys.push_back("popular-"+std::to_string(i));
    std::mt19937_64 rng(tid+1234);
    std::uniform_int_distribution<int> dist(0,(int)w.popular_size-1);
    while(!stop_flag.load() && steady_clock::now()<end){
        std::string key=keys[dist(rng)]; uint64_t lat;
        bool ok=attempt_op(cli,"GET","/kv/"+key,"",w.retries,lat);
        total_requests.fetch_add(1, std::memory_order_relaxed);
        if(ok){total_success.fetch_add(1, std::memory_order_relaxed); total_latency_ns.fetch_add(lat, std::memory_order_relaxed);} else total_errors.fetch_add(1, std::memory_order_relaxed);
    }
}

void run_mix(WorkArgs w){
    int tid=w.tid; auto end=w.end; auto cli=w.cli; uint64_t seq=0;
    std::mt19937_64 rng(tid+999);
    std::uniform_real_distribution<double> ud(0.0,1.0);
    while(!stop_flag.load() && steady_clock::now()<end){
        double r=ud(rng); std::string type;
        if(r<0.05) type="DELETE"; else type=(r<0.7?"GET":"PUT");
        std::string key=key_thread(tid,seq++,w.keyspace);
        json j; j["value"]="v"+std::to_string(seq);
        uint64_t lat; bool ok=attempt_op(cli,type,"/kv/"+key,j.dump(),w.retries,lat);
        total_requests.fetch_add(1, std::memory_order_relaxed);
        if(ok){total_success.fetch_add(1, std::memory_order_relaxed); total_latency_ns.fetch_add(lat, std::memory_order_relaxed);} else total_errors.fetch_add(1, std::memory_order_relaxed);
    }
}


int main(int argc,char**argv){
    if(argc<6){
        std::cout<<"Usage: ./loadgen_new <ip> <port> <clients> <duration> <workload>\n";
        return 1;
    }
    std::string ip=argv[1];
    int port=std::stoi(argv[2]);
    int clients=std::stoi(argv[3]);
    int duration=std::stoi(argv[4]);
    std::string workload=argv[5];

    std::signal(SIGINT,signal_handler);
    std::signal(SIGTERM,signal_handler);

    std::vector<std::thread> th;
    th.reserve(clients);

    auto end=steady_clock::now()+seconds(duration);

    for(int i=0;i<clients;i++){
        auto cli=new httplib::Client(ip,port);
        cli->set_read_timeout(5,0);
        cli->set_write_timeout(5,0);
        WorkArgs w{i,cli,end,100000,100,2};
        if(workload=="get_all") th.emplace_back(run_get_all,w);
        else if(workload=="put_all") th.emplace_back(run_put_all,w);
        else if(workload=="get_popular") th.emplace_back(run_get_popular,w);
        else th.emplace_back(run_mix,w);
    }

    for(auto &t:th) if(t.joinable()) t.join();

    double tp=(double)total_success.load()/duration;
    double avg_ms=(total_success.load()>0? (double)total_latency_ns.load()/1e6/total_success.load():0);

    std::cout<<"Total req: "<<total_requests.load()<<"\n";
    std::cout<<"Success: "<<total_success.load()<<" Errors: "<<total_errors.load()<<"\n";
    std::cout<<"Throughput: "<<tp<<" req/s\n";
    std::cout<<"Avg Latency: "<<avg_ms<<" ms\n";

    // Append results to CSV (clients, throughput, avg_latency_ms)
    try {
        const std::string fname = "results.csv";
        bool write_header = true;
        if (std::filesystem::exists(fname)) {
            if (std::filesystem::file_size(fname) > 0) write_header = false;
        }
        std::ofstream fout(fname, std::ios::app);
        if (!fout) {
            std::cerr << "Failed to open " << fname << " for writing\n";
            return 0;
        }
        if (write_header) {
            fout << "clients,throughput,avg_latency_ms\n";
        }
        fout << clients << "," << tp << "," << avg_ms << "\n";
        fout.close();
        std::cout << "Appended results to " << fname << "\n";
    } catch (const std::exception &e) {
        std::cerr << "CSV write error: " << e.what() << "\n";
    }

    return 0;
}
