#include "counter.hpp"

namespace vg {

Counter::Counter(void) : xgidx(nullptr) { }

Counter::Counter(xg::XG* xidx, size_t binsz) : xgidx(xidx), bin_size(binsz) {
    coverage_dynamic = gcsa::CounterArray(xgidx->seq_length, 8);
    n_bins = xgidx->seq_length / bin_size + 1;
}

Counter::~Counter(void) {
    close_edit_tmpfiles();
    remove_edit_tmpfiles();
}

void Counter::load_from_file(const string& file_name) {
    ifstream in(file_name);
    load(in);
    is_compacted = true;
}

void Counter::save_to_file(const string& file_name) {
    ofstream out(file_name);
    serialize(out);
}

void Counter::load(istream& in) {
    sdsl::read_member(bin_size, in);
    sdsl::read_member(n_bins, in);
    coverage_civ.load(in);
    edit_csas.resize(n_bins);
    for (size_t i = 0; i < n_bins; ++i) {
        edit_csas[i].load(in);
    }
}

void Counter::merge_from_files(const vector<string>& file_names) {
    // load into our dynamic structures, then compact
    ensure_edit_tmpfiles_open();
    for (auto& file_name : file_names) {
        Counter c;
        ifstream f(file_name);
        c.load(f);
        c.write_edits(tmpfstreams);
        collect_coverage(c);
    }
}

size_t Counter::bin_for_position(size_t i) const {
    if (bin_size > 0) {
        return i / bin_size;
    } else {
        return 0;
    }
}

void Counter::write_edits(vector<ofstream>& out) const {
    for (size_t i = 0; i < n_bins; ++i) {
        write_edits(out[i], i);
    }
}

void Counter::write_edits(ostream& out, size_t bin) const {
    out << extract(edit_csas[bin], 0, edit_csas[bin].size()-2) << delim1; // chomp trailing null, add back delim
}

void Counter::collect_coverage(const Counter& c) {
    // assume the same basis vector
    for (size_t i = 0; i < c.graph_length(); ++i) {
        coverage_dynamic.increment(i, c.coverage_at_position(i));
    }
}

size_t Counter::serialize(std::ostream& out,
                          sdsl::structure_tree_node* s,
                          std::string name) {
    make_compact();
    sdsl::structure_tree_node* child = sdsl::structure_tree::add_child(s, name, sdsl::util::class_name(*this));
    size_t written = 0;
    written += sdsl::write_member(bin_size, out, child, "bin_size_" + name);
    written += sdsl::write_member(edit_csas.size(), out, child, "n_bins_" + name);
    written += coverage_civ.serialize(out, child, "graph_coverage_" + name);
    for (auto& edit_csa : edit_csas) {
        written += edit_csa.serialize(out, child, "edit_csa_" + name);
    }
    sdsl::structure_tree::add_size(child, written);
    return written;
}

void Counter::make_compact(void) {
    // pack the dynamic countarry and edit coverage into the compact data structure
    if (is_compacted) return;
    // sync edit file
    close_edit_tmpfiles();
    // temporaries for construction
    size_t basis_length = coverage_dynamic.size();
    int_vector<> coverage_iv;
    util::assign(coverage_iv, int_vector<>(basis_length));
    for (size_t i = 0; i < coverage_dynamic.size(); ++i) {
        coverage_iv[i] = coverage_dynamic[i];
    }
    edit_csas.resize(edit_tmpfile_names.size());
    util::assign(coverage_civ, coverage_iv);
    for (size_t i = 0; i < edit_tmpfile_names.size(); ++i) {
        construct(edit_csas[i], edit_tmpfile_names[i], 1);
    }
    // construct the record marker bitvector
    remove_edit_tmpfiles();
    is_compacted = true;
}

void Counter::make_dynamic(void) {
    if (!is_compacted) return;
    // unpack the compact represenation into the countarray
    assert(false); // not implemented
    is_compacted = false;
}

void Counter::ensure_edit_tmpfiles_open(void) {
    if (tmpfstreams.empty()) {
        string base = ".vg-counter";
        string edit_tmpfile_name = tmpfilename(base);
        std::remove(edit_tmpfile_name.c_str()); // remove this; we'll use it as a base name
        // for as many bins as we have, make a temp file
        tmpfstreams.resize(n_bins);
        edit_tmpfile_names.resize(n_bins);
        for (size_t i = 0; i < n_bins; ++i) {
            edit_tmpfile_names[i] = edit_tmpfile_name+"_"+convert(i);
            tmpfstreams[i].open(edit_tmpfile_names[i]);
            assert(tmpfstreams[i].is_open());
        }
    }
}

void Counter::close_edit_tmpfiles(void) {
    if (!tmpfstreams.empty()) {
        for (auto& tmpfstream : tmpfstreams) {
            tmpfstream << delim1; // pad
            tmpfstream.close();
        }
        tmpfstreams.clear();
    }
}

void Counter::remove_edit_tmpfiles(void) {
    if (!edit_tmpfile_names.empty()) {
        for (auto& name : edit_tmpfile_names) {
            std::remove(name.c_str());
        }
        edit_tmpfile_names.clear();
    }
}

void Counter::add(const Alignment& aln, bool record_edits) {
    // open tmpfile if needed
    ensure_edit_tmpfiles_open();
    // count the nodes, edges, and edits
    for (auto& mapping : aln.path().mapping()) {
        if (!mapping.has_position()) continue;
        size_t i = position_in_basis(mapping.position());
        for (auto& edit : mapping.edit()) {
            if (edit_is_match(edit)) {
                if (mapping.position().is_reverse()) {
                    for (size_t j = 0; j < edit.from_length(); ++j) {
                        coverage_dynamic.increment(i-j);
                    }
                } else {
                    for (size_t j = 0; j < edit.from_length(); ++j) {
                        coverage_dynamic.increment(i+j);
                    }
                }
            } else if (record_edits) {
                // we represent things on the forward strand
                string pos_repr = pos_key(i);
                string edit_repr = edit_value(edit, mapping.position().is_reverse());
                size_t bin = bin_for_position(i);
                tmpfstreams[bin] << pos_repr << edit_repr;
            }
            if (mapping.position().is_reverse()) {
                i -= edit.from_length();
            } else {
                i += edit.from_length();
            }
        }
    }
}

// find the position on the forward strand in the sequence vector
size_t Counter::position_in_basis(const Position& pos) const {
    // get position on the forward strand
    if (pos.is_reverse()) {
        return (int64_t)xg_node_start(pos.node_id(), xgidx)
            + (int64_t)reverse(pos, xg_node_length(pos.node_id(), xgidx)).offset() - 1;
    } else {
        return (int64_t)xg_node_start(pos.node_id(), xgidx) + (int64_t)pos.offset();
    }
}

string Counter::pos_key(size_t i) const {
    Position pos;
    size_t offset = 2;
    pos.set_node_id(i+offset);
    string pos_repr;
    pos.SerializeToString(&pos_repr);
    stringstream s;
    s << delim1 << delim2 << delim1 << escape_delims(pos_repr);
    return s.str();
}

string Counter::edit_value(const Edit& edit, bool revcomp) const {
    string edit_repr;
    if (revcomp) {
        reverse_complement_edit(edit).SerializeToString(&edit_repr);
    } else {
        edit.SerializeToString(&edit_repr);
    }
    stringstream s;
    s << delim1 << escape_delims(edit_repr);
    return s.str();
}

string Counter::escape_delims(const string& s) const {
    return escape_delim(escape_delim(s, delim1), delim2);
}

string Counter::unescape_delims(const string& s) const {
    return unescape_delim(unescape_delim(s, delim1), delim2);
}

string Counter::escape_delim(const string& s, char d) const {
    string escaped; escaped.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        escaped.push_back(c);
        if (c == d) escaped.push_back(c);
    }
    return escaped;
}

string Counter::unescape_delim(const string& s, char d) const {
    string unescaped; unescaped.reserve(s.size());
    for (size_t i = 0; i < s.size()-1; ++i) {
        char c = s[i];
        char b = s[i+1];
        if (c == d && b == d) {
            unescaped.push_back(c);
        } else {
            unescaped.push_back(c);
            if (i == s.size()-2) unescaped.push_back(b);
        }
    }
    return unescaped;
}

size_t Counter::graph_length(void) const {
    return coverage_civ.size();
}

size_t Counter::coverage_at_position(size_t i) const {
    return coverage_civ[i];
}

vector<Edit> Counter::edits_at_position(size_t i) const {
    vector<Edit> edits;
    if (i == 0) return edits;
    string key = pos_key(i);
    size_t bin = bin_for_position(i);
    auto& edit_csa = edit_csas[bin];
    auto occs = locate(edit_csa, key);
    for (size_t i = 0; i < occs.size(); ++i) {
        // walk from after the key and delim1 to the next end-sep
        size_t b = occs[i] + key.size() + 1;
        size_t e = b;
        // look for an odd number of delims
        // run until we find a delim
        while (true) {
            while (extract(edit_csa, e, e)[0] != delim1) ++e;
            // now we are matching the delim... count them
            size_t f = e;
            while (extract(edit_csa, f, f)[0] == delim1) ++f;
            size_t c = f - e;
            e = f; // set pointer to last delim
            if (c % 2 != 0) {
                break;
            }
        }
        string value = unescape_delims(extract(edit_csa, b, e));
        Edit edit;
        edit.ParseFromString(value);
        edits.push_back(edit);
    }
    return edits;
}

ostream& Counter::as_table(ostream& out, bool show_edits) {
    // write the coverage as a vector
    for (size_t i = 0; i < coverage_civ.size(); ++i) {
        out << i << "\t" << coverage_civ[i];
        if (show_edits) {
            out << "\t" << count(edit_csas[bin_for_position(i)], pos_key(i));
            for (auto& edit : edits_at_position(i)) out << " " << pb2json(edit);
        }
        out << endl;
    }
}

ostream& Counter::show_structure(ostream& out) {
    out << coverage_civ << endl; // graph coverage (compacted coverage_dynamic)
    for (auto& edit_csa : edit_csas) {
        out << edit_csa << endl;
    }
    //out << " i SA ISA PSI LF BWT    T[SA[i]..SA[i]-1]" << endl;
    //csXprintf(cout, "%2I %2S %3s %3P %2p %3B   %:3T", edit_csa);
    return out;
}

}