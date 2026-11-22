#include <iostream>
#include <string>
#include <vector>
#include <utility>
#include <map>
#include <fstream>
#include <sstream>

std::vector<int> merge_once (const std::vector<int>& ids,
                            std::pair<int, int> best_pair, int new_id){
    std::vector <int> merged;
        for(std::vector<int>::size_type i = 0; i < ids.size() ; ){
            const bool matches = i + 1 < ids.size()
                 && ids[i] == best_pair.first
                 && ids[i+1] == best_pair.second;

            if(matches){
                merged.push_back(new_id);
                i+= 2;
            } else {
                merged.push_back(ids[i]);
                i+= 1;
            }
            }

return merged;
                            }

#ifdef MINI_BPE_PCRE2
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace corpus_bpe {

using Pair = std::pair<int, int>;
using Frequency = std::int64_t;
constexpr int special_id = 256;
constexpr int first_merge_id = special_id + 1;
const std::string marker = "<|endoftext|>";
const std::string model_header = "mini-bpe-gpt2-v1";

struct PairHash {
    std::size_t operator()(const Pair& pair) const {
        const auto key = (static_cast<std::uint64_t>(pair.first) << 32) |
                         static_cast<std::uint32_t>(pair.second);
        return std::hash<std::uint64_t>{}(key);
    }
};

std::vector<int> byte_ids(const std::string& piece) {
    std::vector<int> ids;
    ids.reserve(piece.size());
    for (unsigned char byte : piece) {
        ids.push_back(byte);
    }
    return ids;
}

struct Model {
    std::vector<std::string> vocab;
    std::vector<Pair> rules;
    std::unordered_map<Pair, int, PairHash> ranks;

    Model() {
        for (int byte = 0; byte < special_id; ++byte) {
            vocab.emplace_back(1, static_cast<char>(byte));
        }
        vocab.push_back(marker);
    }

    void add_rule(Pair pair) {
        const int id = static_cast<int>(vocab.size());
        if (pair.first < 0 || pair.first >= id || pair.first == special_id ||
            pair.second < 0 || pair.second >= id || pair.second == special_id ||
            id == std::numeric_limits<int>::max() || ranks.count(pair) != 0) {
            throw std::runtime_error("Invalid merge in model");
        }
        vocab.push_back(vocab[pair.first] + vocab[pair.second]);
        rules.push_back(pair);
        ranks.emplace(pair, id);
    }
};

using RegexCode = std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>;
using MatchData = std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>;

class Pretokenizer {
    RegexCode regex{nullptr, &pcre2_code_free};
    MatchData match{nullptr, &pcre2_match_data_free};

    template <class Piece>
    void split(const std::string& text, Piece& emit) {
        const auto* bytes = reinterpret_cast<PCRE2_SPTR>(text.data());
        std::size_t offset = 0;
        while (offset < text.size()) {
            // The first match validates the whole UTF-8 document.
            const auto options = offset == 0 ? 0u : PCRE2_NO_UTF_CHECK;
            const int result = pcre2_match(regex.get(), bytes, text.size(), offset,
                                           options, match.get(), nullptr);
            if (result < 0) {
                throw std::runtime_error("Cannot pretokenize UTF-8 at byte " +
                                         std::to_string(offset) + " of the current document");
            }
            const auto* range = pcre2_get_ovector_pointer(match.get());
            emit(text.data() + range[0], range[1] - range[0]);
            offset = range[1];
        }
    }

public:
    Pretokenizer() {
        const char* pattern =
            R"('s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+)";
        int error = 0;
        PCRE2_SIZE offset = 0;
        regex.reset(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern),
                                  PCRE2_ZERO_TERMINATED, PCRE2_UTF | PCRE2_UCP | PCRE2_ANCHORED,
                                  &error, &offset, nullptr));
        if (!regex) {
            throw std::runtime_error("Cannot compile GPT-2 pretokenization pattern");
        }
        pcre2_jit_compile(regex.get(), PCRE2_JIT_COMPLETE);
        match.reset(pcre2_match_data_create_from_pattern(regex.get(), nullptr));
        if (!match) {
            throw std::runtime_error("Cannot allocate regex match data");
        }
    }

    template <class Piece, class Special>
    void read(std::istream& input, Piece emit, Special emit_special) {
        std::string document;
        std::string line;
        while (std::getline(input, line)) {
            std::size_t start = 0;
            for (std::size_t pos = line.find(marker); pos != std::string::npos;
                 pos = line.find(marker, start)) {
                document.append(line, start, pos - start);
                split(document, emit);
                document.clear();
                emit_special();
                start = pos + marker.size();
            }
            document.append(line, start, std::string::npos);
            if (!input.eof()) {
                document.push_back('\n');
            }
        }
        if (!input.eof() || input.bad()) {
            throw std::runtime_error("Failed to read input text");
        }
        split(document, emit);
    }
};

struct WeightedPiece {
    std::vector<int> ids;
    Frequency occurrences;
};

struct PairStats {
    Frequency count = 0;
    std::unordered_set<std::size_t> pieces;
};

using RankedPair = std::pair<Frequency, Pair>;

struct RankOrder {
    const std::vector<std::string>& vocab;

    bool operator()(const RankedPair& left, const RankedPair& right) const {
        if (left.first != right.first) {
            return left.first > right.first;
        }
        const auto& a = left.second;
        const auto& b = right.second;
        if (vocab[a.first] != vocab[b.first]) {
            return vocab[a.first] > vocab[b.first];
        }
        if (vocab[a.second] != vocab[b.second]) {
            return vocab[a.second] > vocab[b.second];
        }
        return a > b;
    }
};

Model train(std::istream& input, int target_size) {
    Model model;
    Pretokenizer pretokenizer;
    std::vector<WeightedPiece> pieces;
    {
        std::unordered_map<std::string, Frequency> frequencies;
        pretokenizer.read(input, [&](const char* piece, std::size_t length) {
            ++frequencies[std::string(piece, length)];
        }, [] {});
        pieces.reserve(frequencies.size());
        for (const auto& entry : frequencies) {
            if (entry.first.size() >= 2) {
                pieces.push_back({byte_ids(entry.first), entry.second});
            }
        }
    }

    std::unordered_map<Pair, PairStats, PairHash> stats;
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const auto& piece = pieces[index];
        for (std::size_t i = 1; i < piece.ids.size(); ++i) {
            auto& entry = stats[{piece.ids[i - 1], piece.ids[i]}];
            entry.count += piece.occurrences;
            entry.pieces.insert(index);
        }
    }

    std::set<RankedPair, RankOrder> ranked{RankOrder{model.vocab}};
    for (const auto& entry : stats) {
        ranked.emplace(entry.second.count, entry.first);
    }

    while (static_cast<int>(model.vocab.size()) < target_size && !ranked.empty()) {
        const Pair best = ranked.begin()->second;
        const int new_id = static_cast<int>(model.vocab.size());
        model.add_rule(best);

        const auto& members = stats.at(best).pieces;
        const std::vector<std::size_t> affected(members.begin(), members.end());
        // Aggregate changes before updating the frequency ranking.
        std::unordered_map<Pair, Frequency, PairHash> deltas;
        for (std::size_t index : affected) {
            auto& piece = pieces[index];
            for (std::size_t i = 1; i < piece.ids.size(); ++i) {
                const Pair pair{piece.ids[i - 1], piece.ids[i]};
                stats.at(pair).pieces.erase(index);
                deltas[pair] -= piece.occurrences;
            }
            piece.ids = merge_once(piece.ids, best, new_id);
            for (std::size_t i = 1; i < piece.ids.size(); ++i) {
                const Pair pair{piece.ids[i - 1], piece.ids[i]};
                stats[pair].pieces.insert(index);
                deltas[pair] += piece.occurrences;
            }
        }
        for (const auto& delta : deltas) {
            auto& entry = stats.at(delta.first);
            if (entry.count > 0) {
                ranked.erase({entry.count, delta.first});
            }
            entry.count += delta.second;
            if (entry.count == 0) {
                stats.erase(delta.first);
            } else {
                ranked.emplace(entry.count, delta.first);
            }
        }
    }
    return model;
}

int parse_integer(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
        throw std::runtime_error("Expected a nonnegative integer: " + value);
    }
    std::size_t parsed = 0;
    const auto result = std::stoll(value, &parsed, 10);
    if (parsed != value.size() || result > std::numeric_limits<int>::max()) {
        throw std::runtime_error("Integer out of range: " + value);
    }
    return static_cast<int>(result);
}

void save_model(const Model& model, const char* path) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error(std::string("Cannot open model for writing: ") + path);
    }
    output << model_header << '\n';
    for (const auto& rule : model.rules) {
        output << rule.first << ' ' << rule.second << '\n';
    }
    output.close();
    if (!output) {
        throw std::runtime_error(std::string("Cannot write model: ") + path);
    }
}

Model load_model(const char* path) {
    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!std::getline(input, line) || line != model_header) {
        throw std::runtime_error(std::string("Cannot read GPT-2-style model header: ") + path);
    }
    Model model;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string left, right, extra;
        if (!(row >> left >> right) || row >> extra) {
            throw std::runtime_error("Expected two token IDs per merge row");
        }
        model.add_rule({parse_integer(left), parse_integer(right)});
    }
    if (!input.eof() || input.bad()) {
        throw std::runtime_error(std::string("Cannot read model: ") + path);
    }
    return model;
}

void encode_piece(const Model& model, const char* piece, std::size_t length,
                  std::vector<int>& ids) {
    ids.clear();
    ids.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        ids.push_back(static_cast<unsigned char>(piece[i]));
    }

    for (;;) {
        int best_id = std::numeric_limits<int>::max();
        Pair best{};
        for (std::size_t i = 1; i < ids.size(); ++i) {
            const auto rule = model.ranks.find({ids[i - 1], ids[i]});
            if (rule != model.ranks.end() && rule->second < best_id) {
                best = rule->first;
                best_id = rule->second;
            }
        }
        if (best_id == std::numeric_limits<int>::max()) {
            break;
        }
        ids = merge_once(ids, best, best_id);
    }
}

struct CachedPiece {
    std::string bytes;
    std::vector<int> ids;
};

std::uint64_t hash_piece(const char* piece, std::size_t length) {
    // FNV-1a hashes the original bytes without constructing a string.
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= static_cast<unsigned char>(piece[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <class Emit>
void encode_tokens(const Model& model, Pretokenizer& pretokenizer, std::istream& input,
                   Emit emit) {
    constexpr std::size_t cache_size = 16384;
    constexpr std::size_t cacheable_length = 64;
    // Each slot stores one exact piece. Collisions replace entries, and long pieces
    // bypass the cache so arbitrary input cannot grow it past its fixed limits.
    std::vector<CachedPiece> cache(cache_size);
    std::vector<int> scratch;

    pretokenizer.read(input, [&](const char* piece, std::size_t length) {
        if (length == 1) {
            emit(static_cast<unsigned char>(piece[0]));
            return;
        }

        const std::vector<int>* ids = &scratch;
        if (length <= cacheable_length) {
            auto& entry = cache[hash_piece(piece, length) % cache_size];
            if (entry.bytes.size() != length || entry.bytes.compare(0, length, piece, length) != 0) {
                encode_piece(model, piece, length, entry.ids);
                entry.bytes.assign(piece, length);
            }
            ids = &entry.ids;
        } else {
            encode_piece(model, piece, length, scratch);
        }

        for (int id : *ids) {
            emit(id);
        }
    }, [&] { emit(special_id); });
}

void encode(const Model& model, std::istream& input, std::ostream& output) {
    Pretokenizer pretokenizer;
    encode_tokens(model, pretokenizer, input, [&](int id) {
        output << id << (id == special_id ? '\n' : ' ');
    });
}

void decode(const Model& model, std::istream& input, std::ostream& output) {
    std::string token;
    while (input >> token) {
        const int id = parse_integer(token);
        if (static_cast<std::size_t>(id) >= model.vocab.size()) {
            throw std::runtime_error("Token ID out of range: " + token);
        }
        output << model.vocab[id];
    }
    if (!input.eof() || input.bad()) {
        throw std::runtime_error("Failed to read token IDs");
    }
}

int run(int argc, char* argv[], const std::string& mode) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        if (mode == "train") {
            if (argc != 4 && argc != 5) {
                throw std::runtime_error("Usage: mini_bpe train INPUT MODEL [VOCAB_SIZE]");
            }
            const int target_size = argc == 5 ? parse_integer(argv[4]) : 10000;
            if (target_size < first_merge_id) {
                throw std::runtime_error("VOCAB_SIZE must be at least 257");
            }
            std::ifstream input(argv[2], std::ios::binary);
            if (!input) {
                throw std::runtime_error(std::string("Cannot open training input: ") + argv[2]);
            }
            const Model model = train(input, target_size);
            save_model(model, argv[3]);
            std::cerr << "Vocabulary: " << model.vocab.size() << " / " << target_size
                      << ", merges: " << model.rules.size() << '\n';
        } else {
            if (argc != 3) {
                throw std::runtime_error("Usage: mini_bpe " + mode + " MODEL");
            }
            const Model model = load_model(argv[2]);
            if (mode == "encode") {
                encode(model, std::cin, std::cout);
            } else {
                decode(model, std::cin, std::cout);
            }
            std::cout.flush();
            if (!std::cout) {
                throw std::runtime_error("Cannot write output");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

}  // namespace corpus_bpe
#endif

int main(int argc , char*argv[]){
    if (argc > 1) {
        const std::string mode = argv[1];
        if (mode == "train" || ((mode == "encode" || mode == "decode") && argc > 2)) {
#ifdef MINI_BPE_PCRE2
            return corpus_bpe::run(argc, argv, mode);
#else
            std::cerr << "Build with -DMINI_BPE_PCRE2 and link PCRE2 for corpus commands\n";
            return 1;
#endif
        }
    }

 if(argc > 1 && std::string(argv[1]) == "decode"){

       std::vector<std::string> vocab;

       for(unsigned int i = 0;i <= 255;i++ ){
        vocab.push_back(std::string(1,static_cast<char>(i)));
       }
       std::ifstream rule_file ("output/rules.txt");
       if(!rule_file){
        std::cerr << "Cannot open output/rules.txt"<<'\n';
        return 1;
       }
       int left , right;
       while(rule_file >> left >> right){
       vocab.push_back(vocab[left] + vocab[right]);
       }
       std::string id_line;
       while(std::getline(std::cin,id_line)){
        std::istringstream id_stream(id_line);
        int id;
        while(id_stream >> id){
            if(id < 0 || id >=static_cast<int>(vocab.size())){
                std::cerr << id;

                return 1;
            }else{
                std::cout << vocab[id];
            }
        }
        std::cout << '\n';
       }
        return 0;
 }

 if(argc > 1 && std::string(argv[1]) == "encode"){

        std::ifstream  rule_file ("output/rules.txt");
 if(!rule_file){
    std::cerr << "Cannot open output/rules.txt"<<'\n';
    return 1;
}

std::vector <std::pair<int,int>> loaded_rules;
int left,right;

while(rule_file >> left >> right){
    loaded_rules.push_back({left,right});
    }

std::string text;
while(std::getline(std::cin,text)){
    std::vector <int> encoded;
    for(unsigned char i : text){
        encoded.push_back(static_cast <int>(i));
    }
    int new_id = 256;
    for(const auto& rule : loaded_rules){
        encoded = merge_once(encoded,rule,new_id);
        new_id += 1;
    }

    for(int i : encoded){
    std::cout << i <<' ';
    }
    std::cout << '\n';
    }
        return 0;
    }

    if(argc > 1 && std::string(argv[1]) == "count"){
        if(argc != 3){
            std::cerr << "Usage: ./mini_bpe count <input_file>" << '\n';
            return 1;
        }
        std::vector<std::pair<int,int>> rules;
for(int round = 0; round < 3; ++round){
        std::ifstream input_file(argv[2]);
        if(!input_file){
            std::cerr << "Cannot open " << argv[2] << '\n';
            return 1;
        }
        std::map<std::pair<int,int>,int> doc_counts;
        std::string lines;
        while(std::getline(input_file,lines)){

        std::vector<int> doc_ids;
        doc_ids.reserve(lines.size());

        for(unsigned char line : lines){
            doc_ids.push_back(static_cast<int>(line));
         }

         int new_id = 256;
    for(const auto& rule : rules){
        doc_ids = merge_once(doc_ids,rule,new_id);
        new_id += 1;
    }
    
        for(std::vector<int>::size_type i = 0 ; i+1 < doc_ids.size();i++){

        std::pair<int,int> pairs{doc_ids[i] , doc_ids[i+1]};

        doc_counts[pairs]++;
        
         }

        }
        std::pair<int,int> best_pair;
    int best_pair_number = 0;

    for(const auto& entry : doc_counts){

        if(entry.second > best_pair_number){
            best_pair_number = entry.second;
            best_pair = entry.first;

        }
    }
    if(best_pair_number > 0){
        std::cerr << best_pair.first << ' ' << best_pair.second
          << ' ' << best_pair_number << '\n';
          rules.push_back(best_pair);
    }else{
        break;
    }
}

for(const auto& rule : rules){
    std::cout << rule.first << ' ' << rule.second << '\n';
}
        return 0;
    }

    std::string lines;
    std::vector <std::vector<int>> docs;
    while(std::getline(std::cin,lines)){

        std::vector<int> doc_ids;
        doc_ids.reserve(lines.size());

        for(unsigned char line : lines){
            doc_ids.push_back(static_cast<int>(line));
        }

        docs.push_back(std::move(doc_ids));
    }

    std::vector <std::pair<int,int>> rules;

    for(int round = 0 ; round < 3 ; ++round){
    std::map<std::pair<int,int>,int>doc_counts;

    for(const auto& doc : docs){

        for(std::vector<int>::size_type i = 0 ; i+1 < doc.size();i++){

        std::pair<int,int> pairs{doc[i] , doc[i+1]};

        doc_counts[pairs]++;
        
         }
    }
    std::pair<int,int> best_pair;
    int best_pair_number = 0;

    for(const auto& entry : doc_counts){

        if(entry.second > best_pair_number){
            best_pair_number = entry.second;
            best_pair = entry.first;

        }
    }

    if(best_pair_number > 0){
        const int new_id = 256+round;
        for (auto& ids : docs){
           ids = merge_once (ids,best_pair,new_id);
        }
    }else {
        break;
    }

    rules.push_back(best_pair);

    }

    for(std::vector<int>::size_type i = 0 ; i<rules.size() ; i++){
        std::cerr << rules[i].first << ' ' << rules[i].second << '\n';
    }

    for (const auto& doc : docs) {
    for (int id : doc) {
        std::cout << id << ' ';
    }
    std::cout << '\n';
    }

    return 0;
}
