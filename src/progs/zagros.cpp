/*
 *    Copyright (C) 2013 University of Southern California and
 *                       Andrew D. Smith
 *
 *    Authors: Emad Bahrami-Samani, Philip J. Uren and Andrew D. Smith
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// STL includes
#include <cassert>
#include <ctime>
#include <sys/time.h>
#include <string>
#include <vector>
#include <iostream>
#include <iterator>
#include <numeric>
#include <queue>
#include <tr1/unordered_map>

// smithlab common code includes
#include "OptionParser.hpp"
#include "smithlab_utils.hpp"
#include "smithlab_os.hpp"
#include "RNG.hpp"

// Zagros package common code includes
#include "RNA_Utils.hpp"
#include "Model.hpp"
#include "IO.hpp"
#include "Util.hpp"

// OMP
#ifdef _OPENMP
#include <omp.h>
#endif

#include <mpi.h>

using std::tr1::unordered_map;
using std::stringstream;
using std::ifstream;
using std::string;
using std::vector;
using std::cout;
using std::cerr;
using std::endl;
using std::max;
using std::pair;
using std::numeric_limits;


////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
/////////////
/////////////  STUFF FOR FINDING THE STARTING POINT BELOW HERE
/////////////

struct motif_info {
  double motifLogLike;
  Model motifModel;
  size_t motifNumber;
  vector<vector<double> > motifIndicators;
  vector<double> hasMotif;
  motif_info(const double ml, const Model &mm, const size_t mn,
             const vector<vector<double> > &mi, const vector<double> &hm) :
    motifLogLike(ml), motifNumber(mn) {
      motifModel = mm;
      motifIndicators = mi;
      hasMotif = hm;
  }
  double score() const {
    return motifLogLike;
  }
};

struct kmer_info {
  std::string kmer;
  double expected;
  size_t observed;

  kmer_info(const std::string &km, const double ex, const double ob) :
    kmer(km), expected(ex), observed(ob) {}
  double score() const {
    return observed/expected;
  }
  bool operator>(const kmer_info &ki) const {
    double myScore = score();
    double kiScore = ki.score();

    // If no score is available keep the latest kmer
    if ((myScore == 0 && kiScore == 0) || (myScore == kiScore))
      return kmer > ki.kmer;
    else 
      return myScore > kiScore;
  }
};

int wsize, rank;
struct valueProcess {
  double value;
  int process;
};


/**
 * \brief TODO
 * \param prob      TODO
 * \param seq_len   TODO
 * \return TODO
 * this is just Poisson probability for 0 observations
 */
static double
prob_no_occurrence(const double prob,
                   const size_t seq_len) {
  return std::exp(-static_cast<int>(seq_len) * prob);
}

/**
 * \brief TODO
 * \param sequences TODO
 * \param base_comp TODO
 */
static void
compute_base_comp(const vector<string> &sequences,
                  vector<double> &base_comp) {
  base_comp.resize(smithlab::alphabet_size, 0.0);
  size_t total = 0;
  for (size_t i = 0; i < sequences.size(); ++i) {
    for (size_t j = 0; j < sequences[i].length(); ++j) {
      const size_t base = base2int(sequences[i][j]);
      // ADS: poor coding style below -- complement the condition, and
      // remove the continue!!!
      if ((sequences[i][j] == 'N') || (sequences[i][j] == 'n')) continue;
      if (base >= smithlab::alphabet_size) {
        stringstream ss;
        ss << "failed computing base composition, unexpected base: "
           << sequences[i][j];
        throw SMITHLABException(ss.str());
      }
      ++base_comp[base];
    }
    total += sequences[i].length();
  }
  std::transform(base_comp.begin(), base_comp.end(), base_comp.begin(),
		 std::bind2nd(std::divides<double>(), total));
}

/**
 * \brief TODO
 * \param kmer          TODO
 * \param base_comp     TODO
 * \return TODO
 */
static double
compute_kmer_prob(const string &kmer,
                  const vector<double> &base_comp) {
  double prob = 1.0;
  for (size_t i = 0; i < kmer.length(); ++i) {
    const size_t base = base2int(kmer[i]);
    assert(base < smithlab::alphabet_size);
    prob *= base_comp[base];
  }
  return prob;
}

/**
 * \brief TODO
 * \param kmer         TODO
 * \param base_comp    TODO
 * \param lengths      TODO
 * \return TODO
 */
static double
expected_seqs_with_kmer(const string &kmer,
                        const vector<double> &base_comp,
                        const vector<size_t> &lengths) {
  const double p = compute_kmer_prob(kmer, base_comp);
  double expected = 0.0;
  for (size_t i = 0; i < lengths.size(); ++i)
    expected += (1.0 - prob_no_occurrence(p, lengths[i]));
  return expected;
}

/**
 * \brief TODO
 * \param kmer          TODO
 * \param sequences     TODO
 */
static size_t
count_seqs_with_kmer(const string &kmer,
                     const vector<string> &sequences) {
  size_t count = 0;
  for (size_t i = 0; i < sequences.size(); ++i) {
    bool has_kmer = false;
    const size_t lim = sequences[i].length() - kmer.length() + 1;
    for (size_t j = 0; j < lim && !has_kmer; ++j)
      has_kmer = !sequences[i].compare(j, kmer.length(), kmer);
    count += has_kmer;
  }
  return count;
}

/**
 * \brief TODO
 * \param k_value       TODO
 * \param n_top_kmers   TODO
 * \param sequences     TODO
 * \param top_kmers     TODO
 */
static void
find_best_kmers(const size_t k_value,       // Equals to motif width
                const size_t n_top_kmers,   // Equals to num of starting points
                const vector<string> &sequences,
                vector<kmer_info> &top_kmers) {

  const size_t n_kmers = (1ul << 2 * k_value);

  vector<double> base_comp;
  compute_base_comp(sequences, base_comp);

  vector<size_t> lengths;
  for (size_t i = 0; i < sequences.size(); ++i)
    lengths.push_back(sequences[i].length());

  std::priority_queue<kmer_info, vector<kmer_info>, std::greater<kmer_info> > best_kmers;

  #pragma omp parallel for schedule(dynamic)
  for (size_t i = 0; i < n_kmers; ++i) {
    const string kmer(i2mer(k_value, i));

    const double expected = expected_seqs_with_kmer(kmer, base_comp, lengths);
    const size_t observed = count_seqs_with_kmer(kmer, sequences);

    #pragma omp critical
    {
    best_kmers.push(kmer_info(kmer_info(kmer, expected, observed)));
    if (best_kmers.size() > n_top_kmers)
      best_kmers.pop();
    }
  }

  while (!best_kmers.empty()) {
    top_kmers.push_back(best_kmers.top());
    best_kmers.pop();
  }
  reverse(top_kmers.begin(), top_kmers.end());
}

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
/////////////
/////////////  INPUT DATA TEST AND REFINEMENT
/////////////

/***
 * \summary given a set of sequences and indicators for motif occurrences,
 *          mask out the most likely occurrences of the motif in the sequences
 * \param seqs          the sequences to mask occurrences in
 * \param indicators    indicators[i][j] is the prob. that a motif occurrence
 *                      starts at position j of sequence i
 * \param zoops         zoops[i] is the prob. that the ith sequence contains an
 *                      occurrence of the motif.
 * \param motifLen      the length of the motif. i.e. how much to mask.
 * \throw SMITHLABException if the dimensions of seqs doesn't match indicators
 *        or zoops_i
 */
static void
maskOccurrences(vector<string> &seqs, const vector<vector<double> > &indicators,
                const vector<double> &zoops, const size_t motifLen) {
  // we won't consider masking any sequences where the prob. that the motif is
  // in the sequence is lower than this.
  const double ZOOPS_OCCURRENCE_THRESHOLD = 0.8;

  if (seqs.size() != indicators.size()) {
    stringstream ss;
    ss << "failed to mask motif occurrences, number of indicator vectors ("
       << indicators.size() << ") didn't match the number of sequences ("
       << seqs.size() << ")";
    throw SMITHLABException(ss.str());
  }
  if (seqs.size() != zoops.size()) {
    stringstream ss;
    ss << "failed to mask motif occurrences, number of zoops indicators ("
       << zoops.size() << ") didn't match the number of sequences ("
       << seqs.size() << ")";
    throw SMITHLABException(ss.str());
  }

  for (size_t i = 0; i < seqs.size(); ++i) {
    if (zoops[i] >= ZOOPS_OCCURRENCE_THRESHOLD) {
      double max_X = -1;
      int max_indx = -1;
      for (size_t j = 0; j < indicators[i].size(); j++) {
        if (indicators[i][j] > max_X) {
          max_X = indicators[i][j];
          max_indx = j;
        }
      }
      const string mask = string(motifLen, 'N');
      seqs[i].replace(max_indx, motifLen, mask);
    }
  }
}


/******************************************************************************
 *                 CODE FOR FORMATTING MOTIF / MODEL OUTPUT
*******************************************************************************/

/**
 * \brief Format a motif occurrence into a string for output with the motif.
 *        The output will follow this pattern: BS   XXXXX; NAME; A; B; ; C;
 *        where XXXXX is the sequence of the occurrence, NAME is the parameter
 *        <name> provided to this function, A is the parameter <site_pos>
 *        provided to this function, B is the parameter <width> provided to
 *        this function, and C is the parameter <strand> provided to this
 *        function.
 * \param seq       the sequence that this occurrence is in
 * \param width     the length of the motif occurrence
 * \param name      the name of the region the occurrence is in; this should
 *                  be the assembled region name if it's available, but any
 *                  string will do (e.g. if the user provided only fasta input,
 *                  then a sensible choice would be the name of the sequence).
 * \param strand    the strand that this occurrence is on
 * \param site_pow  the relative position (zero-based) within seq that the
 *                  occurrence is at.
 */
static string
format_site(const string &seq, const size_t width, const string &name,
            const char strand, const size_t site_pos) {
  std::ostringstream ss;
  ss << "BS\t" << seq.substr(site_pos, width) << "; " << name << "; "
     << site_pos << "; " << width << ";  ;" << strand;
  return ss.str();
}

/**
 * \brief TODO
 * \param name  TODO
 */
static string
format_motif_header(const string &name) {
  static const string the_rest("XX\nTY\tMotif\nXX\nP0\tA\tC\tG\tT");
  std::ostringstream oss;
  oss << "AC\t" << name << '\n' << the_rest;
  return oss.str();
}

/***
 * \brief TODO
 * \param model         TODO
 * \param motif_name    TODO
 * \param sequences     TODO
 * \param indicators    TODO
 * \param zoops_i       TODO
 * \return  TODO
 * \throw   TODO
 */
static string
format_motif(const Model &model,
             const string &motif_name,
             const vector<string> &sequences,
             const vector<string> &names,
             const vector<GenomicRegion> &targets,
             const vector<vector<double> > &indicators,
             const vector<double> &zoops_i) {

  static const string BLANK_LINE = "XX";
  static const string ATTRIBUTE_TAG = "AT";
  static const double ZOOPS_OCCURRENCE_THRESHOLD = 0.8;

  assert(indicators.size() == sequences.size());

  std::ostringstream ss;
  ss << format_motif_header(motif_name) << endl;

  vector<vector<double> > tmp_m = model.matrix;
  for (size_t n = 0; n < sequences.size(); n++) {
    double max_X = -1;
    int max_i = -1;
    for (size_t i = 0; i < indicators[n].size(); i++) {
      if (indicators[n][i] > max_X) {
        max_X = indicators[n][i];
        max_i = i;
      }
    }
    if (zoops_i[n] >= ZOOPS_OCCURRENCE_THRESHOLD)
      for (size_t j = 0; j < model.size(); ++j) {
        // just skip N's
	// ADS: poor coding style again below here
        if ((sequences[n][max_i + j] == 'N') ||
	    (sequences[n][max_i + j] == 'n'))
          continue;

        const size_t base = base2int(sequences[n][max_i + j]);
        assert(base < smithlab::alphabet_size);
        tmp_m[j][base] += zoops_i[n];
      }
  }

  for (size_t j = 0; j < tmp_m.size(); j++) {
    // TODO fix below
    // AS: this is crazy below and will not work for motifs of width 11
    ss << "0" << j + 1;
    for (size_t b = 0; b < smithlab::alphabet_size; ++b)
      ss << '\t' << static_cast<int>(tmp_m[j][b]);
    ss << endl;
  }

  if (model.motif_sec_str.size() > 0) {
    ss << BLANK_LINE << endl
       << ATTRIBUTE_TAG << '\t' << "SEC_STR=";
    for (size_t i = 0; i < model.motif_sec_str.size() - 1; ++i)
      ss << model.motif_sec_str[i] << ",";
    ss << model.motif_sec_str[model.motif_sec_str.size() - 1] << endl;
  }

  ss << BLANK_LINE << endl;
  if (model.useDEs) {
    ss << ATTRIBUTE_TAG << '\t' << "GEO_P=" << model.p << endl;
    ss << ATTRIBUTE_TAG << '\t' << "GEO_DELTA=" << model.delta << endl
       << BLANK_LINE << endl;
  }

  size_t numSitesFound = 0;
  for (size_t n = 0; n < indicators.size(); ++n) {
    double max_X = -1;
    size_t site_pos = 0;
    for (size_t i = 0; i < indicators[n].size(); i++) {
      if (indicators[n][i] > max_X) {
        max_X = indicators[n][i];
        site_pos = i;
      }
    }
    if (zoops_i[n] >= ZOOPS_OCCURRENCE_THRESHOLD) {
      // assume positive strand, unless we know the region was negative strand
      char strand = '+';
      if (targets.size() != 0) strand = targets[n].get_strand();
      ss << format_site(sequences[n], model.size(), names[n], strand, site_pos)
         << "\t" << zoops_i[n] << endl;
      numSitesFound += 1;
    }
  }
  ss << BLANK_LINE << endl << "//";

  // if we found no sites that actually matched the motif, don't bother
  // to return the empty formatted version.
  if (numSitesFound == 0) return "";
  else return ss.str();
}

int main(int argc, const char **argv) {
  try {
    // TODO -- PJU: what is this?
    static const double zoops_expansion_factor = 0.75;
    static const double GEO_P_DEFAULT = 0.135;

    // options/parameters that the user can set.
    bool VERBOSE = false;
    size_t motif_width = 6;
    size_t n_motifs = 10;
    string outfile;
    string chrom_dir = "";
    string structure_file;
    string reads_file;
    string indicators_file = "";
    double epsilon = 0.0;
    size_t numStartingPoints = 10;
    string delta = "NotApp";
    bool geo = false;
    double de_weight = 1.1;

    /****************** COMMAND LINE OPTIONS ********************/
    // TODO -- PJU: some options below don't have their defaults specified,
    //              they need to be fixed so defaults are shown, but not
    //              hard-coded into the strings. Ditto acceptable ranges.
    OptionParser opt_parse(strip_path(argv[0]), "",
			   "<target_regions/sequences>");
    opt_parse.add_opt("output", 'o', "output file name (default: stdout)",
                      OptionParser::OPTIONAL, outfile);
    opt_parse.add_opt("width", 'w', "width of motifs to find (4 <= w <= 12; "
                      "default: 6)", OptionParser::OPTIONAL, motif_width);
    opt_parse.add_opt("number", 'n', "number of motifs to output (default: 10)",
                      OptionParser::OPTIONAL, n_motifs);
    opt_parse.add_opt("chrom", 'c', "directory with chrom files (FASTA format)",
                      OptionParser::OPTIONAL, chrom_dir);
    opt_parse.add_opt("structure", 't', "structure information file",
                      OptionParser::OPTIONAL, structure_file);
    opt_parse.add_opt("diagnostic_events", 'd',
                      "diagnostic events information file",
                      OptionParser::OPTIONAL, reads_file);
    opt_parse.add_opt("delta", 'l', "provide a fixed value for delta, the "
                      "offset of cross-linking site from motif occurrences. "
                      "-8 <= l <= 8; if omitted, delta is optimised using an "
                      "exhaustive search", OptionParser::OPTIONAL, delta);
    opt_parse.add_opt("geo", 'g', "optimize the geometric distribution"
                      "parameter for the distirbution of cross-link "
                      "sites around motif occurrences, using the "
                      "Newton-Raphson algorithm. If omitted, this "
                      "parameter is not optimised and is set to a "
                      "empirically pre-determined default value.",
                      OptionParser::OPTIONAL, geo);
    opt_parse.add_opt("de_weight", 'k', "A weight to determine the diagnostic events' "
                      "level of contribution (default: 1.1)", OptionParser::OPTIONAL, de_weight);
    opt_parse.add_opt("indicators", 'a', "output indicator probabilities for "
                      "each sequence and motif to this file",
                      OptionParser::OPTIONAL, indicators_file);
    opt_parse.add_opt("starting-points", 's', "number of starting points to try"
                      " for EM search. Higher values will be slower, but more"
                      " likely to find the global maximum (default: 10)",
                      OptionParser::OPTIONAL, numStartingPoints);
    opt_parse.add_opt("verbose", 'v', "print more run info",
                      OptionParser::OPTIONAL, VERBOSE);
    vector<string> leftover_args;
    opt_parse.parse(argc, argv, leftover_args);
    if (argc == 1 || opt_parse.help_requested()) {
      cerr << opt_parse.help_message() << endl << opt_parse.about_message()
	   << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.about_requested()) {
      cerr << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.option_missing()) {
      cerr << opt_parse.option_missing_message() << endl;
      return EXIT_SUCCESS;
    }
    if (motif_width < 4 || motif_width > 12) {
      cerr << "motif width should be between 4 and 12" << endl;
      return EXIT_SUCCESS;
    }
    if ((epsilon < 0) || (epsilon > 1)) {
      cerr << "diagEventsThresh option must be between 0 and 1" << endl;
      return EXIT_SUCCESS;
    }
    if (delta!="0" && atoi(delta.c_str())==0 && delta != "NotApp") {
      cerr << "Delta parameter is not valid!" << endl;
      return EXIT_SUCCESS;
    } else if (delta!="0" && atoi(delta.c_str())!=0 && 
               ((atoi(delta.c_str()) < -8) || (atoi(delta.c_str()) > 8))) {
      cerr << "delta parameter should be between -8 and 8" << endl;
       return EXIT_SUCCESS;
    }
    if (leftover_args.size() != 1) {
      cerr << "Zagros requires one input file, found "
           << leftover_args.size() << ": ";
      for (size_t i = 0; i < leftover_args.size(); ++i) {
        cerr << leftover_args[i];
        if (i != (leftover_args.size() - 1)) cerr << ", ";
      }
      cerr << endl << opt_parse.help_message() << endl;
      return EXIT_SUCCESS;
    }
    const string targets_file(leftover_args.back());
    /****************** END COMMAND LINE OPTIONS *****************/

#ifdef _OPENMP
    if (VERBOSE)
      cerr << "Running zagros with OMP (" << omp_get_max_threads() <<" Threads)" << endl; 
#endif

    // Initialize MPI
    MPI_Init(NULL, NULL);
    MPI_Comm_size(MPI_COMM_WORLD, &wsize);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    const Runif rng;

    std::ofstream of;
    if (!outfile.empty())
      of.open(outfile.c_str());
    std::ostream out(outfile.empty() ? cout.rdbuf() : of.rdbuf());

    // Data structures and input preparation for sequence
    if (VERBOSE)
      cerr << "LOADING SEQUENCES" << endl;
    vector<string> seqs, names;
    vector<GenomicRegion> targets;
    load_sequences(targets_file, chrom_dir, seqs, names, targets);

    // Data structures and input preparation for secondary structure
    vector<vector<double> > secondary_structure;
    if (!structure_file.empty()) {
      if (VERBOSE)
        cerr << "LOADING STRUCTURE INFORMATION" << endl;
      load_structures(structure_file, secondary_structure);
      if (!seq_and_structure_are_consistent(seqs, secondary_structure))
        throw SMITHLABException("inconsistent dimensions of "
				"sequence and structure data");
    }

    // Load the diagnostic events
    vector<vector<double> > diagEvents(seqs.size());
    vector<vector<vector<double> > > diag_values(seqs.size());
    if (!reads_file.empty()) {
      if (VERBOSE)
        cerr << "LOADING DIAGNOSTIC EVENTS... ";
      const double deCount = loadDiagnosticEvents(reads_file, diagEvents,
                                                  diag_values, epsilon,
                                                  de_weight, GEO_P_DEFAULT, motif_width);
      if (diagEvents.size() != seqs.size()) {
        stringstream ss;
        ss << "inconsistent dimensions of sequence and diagnostic events data. "
           << "Found " << seqs.size() << " sequences, and " << diagEvents.size()
           << " diagnostic events vectors";
        throw SMITHLABException(ss.str());
      }
      if (VERBOSE)
        cerr << "DONE (FOUND " << deCount << " EVENTS IN TOTAL)" << endl;
    }


    // find and output each of the motifs that the user asked for.
    vector<motif_info> top_motifs;
    vector<string> original_seqs = seqs;

    for (size_t i = 0; i < n_motifs ; ++i) {
      if (VERBOSE && rank == 0)
        cerr << "FITTING MOTIF PARAMETERS FOR MOTIF " << (i+1)
             << " OF " << n_motifs << endl;

      // pick a set of starting points to try
      vector<kmer_info> top_kmers;
      find_best_kmers(motif_width, numStartingPoints, seqs, top_kmers);

      double bestLogLike = 0;
      valueProcess bestLogLikeL;
      bestLogLikeL.value = 0;
      bestLogLikeL.process = rank;

      bool firstKmer = true;

      // these will hold the best results after trying all of our
      // starting point kmers
      vector<vector<double> > indicators;
      vector<double> has_motif;
      Model model;


      for (size_t j = rank; j < numStartingPoints; j+=wsize) {
        Model model_l;
        Model::set_model_by_word(Model::pseudocount, top_kmers[j].kmer, model_l);
        model_l.de_weight = de_weight;
        if (!geo) {
          model_l.p = GEO_P_DEFAULT;
          model_l.opt_geo = false;
        } else
          model_l.p = 0.5;
        if (delta != "NotApp") {
          model_l.delta = atoi(delta.c_str());
          model_l.opt_delta = false;
        }
        if (!reads_file.empty())
          model_l.useDEs = true;
        model_l.gamma = ((seqs.size() - (zoops_expansion_factor * (seqs.size() - top_kmers[j].observed))) / static_cast<double>(seqs.size()));
       
        if (!secondary_structure.empty()) {
          model_l.useStructure = true;
          model_l.motif_sec_str = vector<double>(motif_width, 0.5);
          model_l.f_sec_str = 0.5;
        }

        vector<double> has_motif_l(seqs.size(), model_l.gamma);
        vector<vector<double> > indicators_l;

        for (size_t k = 0; k < seqs.size(); ++k) {
          const size_t n_pos = seqs[k].length() - motif_width + 1;
          indicators_l.push_back(vector<double>(n_pos, 1.0 / n_pos));
        }

        if (VERBOSE)
          cerr << "\t" << "TRYING STARTING POINT " << (j+1) << " OF "
          << numStartingPoints << " (" << top_kmers[j].kmer << ") ... ";

        model_l.expectationMax(seqs, diagEvents, diag_values, secondary_structure, indicators_l, has_motif_l);

        double logLike;
        if (secondary_structure.size() == 0) {
          logLike = model_l.calculate_zoops_log_l(original_seqs, diagEvents, diag_values, indicators_l, has_motif_l);
        } else {
          logLike = model_l.calculate_zoops_log_l(original_seqs,
						                                      secondary_structure,
                                                  diagEvents, diag_values, indicators_l,
                                                  has_motif_l);
        }
        if (VERBOSE)
          cerr << "LOG-LIKELIHOOD: " << logLike << endl;

        if ((firstKmer) || (logLike > bestLogLikeL.value)) {
          bestLogLikeL.value = logLike;
          model = model_l;
          indicators = indicators_l;
          has_motif = has_motif_l;
          firstKmer = false;
        }
      }

      // Reduce the best kmer
      valueProcess bestLogLikeProcess;
      MPI_Allreduce(&bestLogLikeL, &bestLogLikeProcess, 1, MPI_DOUBLE_INT, MPI_MAXLOC, MPI_COMM_WORLD);
      
      if (rank == 0 && VERBOSE)
        cerr << "BEST LogLike is from process " << bestLogLikeProcess.process << endl;
      
      // Only the process with the best logLike stores the motif to avoid communications
      if (rank == bestLogLikeProcess.process) {
          bestLogLike = bestLogLikeProcess.value;

        if (VERBOSE)
          cerr << "\t" << "WRITING MOTIF " << endl;

        top_motifs.push_back(motif_info(bestLogLike, model, i, indicators, has_motif));

        // if not the last motif, mask occurrences
        if (i != n_motifs - 1) {
          if (VERBOSE) cerr << "\t" << "MASKING MOTIF OCCURRENCES" << endl;
          maskOccurrences(seqs, indicators, has_motif, motif_width);
        }

      }

      // Broadcast sequences with masked ocurrences
      for (size_t j = 0; j < seqs.size(); j++)
        MPI_Bcast((void *) seqs[j].data(), seqs[j].size(), MPI_CHAR, bestLogLikeProcess.process, MPI_COMM_WORLD);

    }

    // TODO: Write the motifs to file sorted by top motif score
    // Loop through process to no write to the file at the same time
    for (int j = 0; j < wsize; j++) {
      if (rank == j) {
        // write out motif occurrences, and collect up the zoops indicators
        // for potential output later.
        stringstream indicators_output;
        while (!top_motifs.empty()) {
          double maxScore = top_motifs[0].score();
          size_t maxIndex = 0;
          for (size_t i = 1; i < top_motifs.size(); ++i)
            if (top_motifs[i].score() > maxScore) {
              maxScore = top_motifs[i].score();
              maxIndex = i;
            }
          
          const string m = format_motif(top_motifs[maxIndex].motifModel,
                                        "ZAGROS" +
                                        toa(top_motifs[maxIndex].motifNumber),
                                        original_seqs, names, targets,
                                        top_motifs[maxIndex].motifIndicators,
                                        top_motifs[maxIndex].hasMotif);

          copy(top_motifs[maxIndex].hasMotif.begin(),
              top_motifs[maxIndex].hasMotif.end(),
              std::ostream_iterator<double>(indicators_output, "\n"));

          if (m.empty() && VERBOSE)
            cerr << "\t" << "WARNING, MOTIF HAD NO OCCURRENCES; SKIPPING" << endl;
          if (!m.empty())
            out << m << endl;
          top_motifs.erase(top_motifs.begin() + maxIndex);
        }

        // write out zoops indicators, if have a filename to output them to
        if (!indicators_file.empty()) {
          std::ofstream ind_fs(indicators_file.c_str());
          ind_fs << indicators_output.rdbuf() << endl;
        }
      }

      // Wait for process to write his motifs
      MPI_Barrier(MPI_COMM_WORLD);
    }
  }
  catch (const SMITHLABException &e) {
    cerr << "ERROR: " << e.what();
    cerr << endl;
    return EXIT_FAILURE;
  }
  catch (std::bad_alloc &ba) {
    cerr << "ERROR: could not allocate memory" << endl;
    return EXIT_FAILURE;
  }

  MPI_Finalize();
  return EXIT_SUCCESS;
}
