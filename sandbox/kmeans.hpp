#ifndef SKYLARK_KMEANS_HPP
#define SKYLARK_KMEANS_HPP

#include <cassert>
#include <time.h>
#include "pretty_printer.hpp"

#include "util.hpp"
#include "dist_matrix.hpp"
#include "thd_safe_bool_vector.hpp"

#define root 0
#define KM_DEBUG 0
#define INVALID_ID UINT_MAX

/*******************************************/
namespace skyutil = skylark::utility;
namespace kpmbase = kpmeans::base;
namespace kpmprune = kpmeans::prune;
/*******************************************/

// Annymous namespace for use only here
namespace {
enum init_t {
    RANDOM,
    FORGY,
    PLUSPLUS,
    BARBAR,
    SKETCH,
    NONE
};

template <typename T>
T euclidean_distance(const T* arg0, const T* arg1, const El::Unsigned len) {
    T dist = 0;
    for (El::Unsigned i = 0; i < len; i++) {
        T _dist = arg0[i] - arg1[i];
        dist += (_dist*_dist);
    }
    return std::sqrt(dist);
}

template <typename T>
void add_sample(El::Matrix<T>& centroids, const El::Unsigned cid,
       const El::Matrix<T>& data, const El::Unsigned sample_id,
       El::Matrix<El::Int>& assignment_count) {
    El::Unsigned dim = centroids.Height();
    centroids(El::IR(0, dim), El::IR(cid, cid+1)) +=
        data(El::IR(0, dim), El::IR(sample_id, sample_id+1));

    assignment_count.Set(0, cid,
            assignment_count.Get(0, cid) + 1);
}

template <typename T>
void remove_sample(El::Matrix<T>& centroids, const El::Unsigned cid,
       const El::Matrix<T>& data, const El::Unsigned sample_id,
       El::Matrix<El::Int>& assignment_count) {
    El::Unsigned dim = centroids.Height();
    centroids(El::IR(0, dim), El::IR(cid, cid+1)) -=
        data(El::IR(0, dim), El::IR(sample_id, sample_id+1));

    assignment_count.Set(0, cid,
            assignment_count.Get(0, cid) - 1);
}

template <typename T>
void get_prev_dist(const El::Matrix<T>& prev_centroids,
        const El::Matrix<T>& centroids, std::vector<T>& prev_dist) {
    assert (prev_dist.size() == centroids.Width());
    for (El::Unsigned cl = 0; cl < centroids.Width(); cl++) {
        prev_dist[cl] = euclidean_distance(prev_centroids.LockedBuffer(0, cl),
                centroids.LockedBuffer(0, cl));
    }
}

/**
  * Inverse of the mean for a col-wise matrix
  */
template <typename T>
void unmean(El::Matrix<T>& mat, const El::Matrix<El::Int>& count) {
    const El::Unsigned dim = mat.Height();
    T* matbuf = mat.Buffer();
    const T* countbuf = count.LockedBuffer();
    assert(dim == count.Width());

    for (El::Unsigned i = 0; i < mat.Width(); i++) {
        for (El::Unsigned j = 0; j < dim; j++) {
            matbuf[i*dim + j] *= countbuf[i]; // TODO: Verify correctness
        }
    }
}

template <typename T>
void col_mean_raw(El::Matrix<T>& mat, El::Matrix<T>& outmat,
        const El::Matrix<El::Int>& counts) {
    assert(counts.Width() == mat.Width());

    T* buf = mat.Buffer();
    T* outbuf = outmat.Buffer();
    const El::Unsigned Height = mat.Height();
    // Division by row
    for (El::Unsigned col = 0; col < mat.Width(); col++) {
        for (El::Unsigned row = 0; row < mat.Height(); row++) {
            // Update global clusters
            if (counts.Get(0, col) > 0)
                outbuf[col*Height + row] =
                    buf[col*Height + row] / counts.Get(0, col);
            else
                outbuf[col*Height + row] = buf[col*Height + row];
        }
    }
}

/**
  Used to generate the a stream of random numbers on every processor but
  allow for a parallel and serial impl to generate identical results.
    NOTE: This only works if the data is distributed to processors in the
  same fashion as <El::VC, El::STAR> or <El::STAR, El::VC>
**/
template <typename T>
class mpi_random_generator {
public:
    // End range (end_range) is inclusive i.e random numbers will be
    //      in the inclusive interval (begin_range, end_range)
    mpi_random_generator(const size_t begin_range,
            const size_t end_range, const El::Unsigned rank,
            const size_t nprocs, const size_t seed) {
        this->_nprocs = nprocs;
        this->_gen = std::default_random_engine(seed);
        this->_rank = rank;
        this->_dist = std::uniform_int_distribution<T>(begin_range, end_range);
        init();
    }

    void init() {
        for (size_t i = 0; i < _rank; i++)
            _dist(_gen);
    }

    T next() {
        T ret = _dist(_gen);
        for (size_t i = 0; i < _nprocs-1; i++)
            _dist(_gen);
        return ret;
    }

private:
    std::uniform_int_distribution<T> _dist;
    std::default_random_engine _gen;
    size_t _nprocs;
    El::Unsigned _rank;
};

template <typename T>
void kmeanspp_init(const El::DistMatrix<T, El::VC, El::STAR>& data,
        El::Matrix<T>& centroids, const El::Unsigned seed,
        const El::Unsigned k) {
#if 0
    El::Matrix<T> local_data = data.LockedMatrix();
    // NOTE: Will fail if every proc doesn't have enough mem for this!
    El::Matrix<T> dist_v(1, data.Height());
    // Only localEntries should be set to max
    El::Fill(dist_v, std::numeric_limits<T>::max());

    // Choose c1 uniformly at random
    srand(seed);
     // This is a global index
    El::Unsigned gl_selected_rid = random() % data.Height();

    if (data.IsLocalRow(gl_selected_rid)) {
        El::Unsigned local_selected_rid = data.LocalRow(gl_selected_rid);
#if KM_DEBUG
        El::Output("Proc: ", data.DistRank(), " assigning global r:",
                gl_selected_rid, ", local r: ", local_selected_rid,
                " as centroid: 0");
#endif
        // Add row to local clusters
        centroids(El::IR(0,1), El::IR(0, data.Width())) +=
            local_data(El::IR(local_selected_rid, (local_selected_rid+1)),
                    El::IR(0, data.Width()));
        dist_v.Set(0, gl_selected_rid, 0);
    }

    // Globally sync across processes
    El::AllReduce(centroids, El::mpi::COMM_WORLD, El::mpi::SUM);

    unsigned clust_idx = 0; // The number of clusters assigned

    // Choose next center c_i with weighted prob
    while ((clust_idx + 1) < k) {
        T cuml_dist = 0;
        for (size_t row = 0; row < local_data.Height(); row++) {
            // Do a distance step
            T dist = euclidean_distance<T>(local_data.LockedBuffer(row, 0),
                    local_data.Height(),
                    centroids.LockedBuffer(clust_idx, 0), centroids.Height(),
                    centroids.Width());
            if (dist < dist_v.Get(0, data.GlobalRow(row)))
                dist_v.Set(0, data.GlobalRow(row), dist);
            cuml_dist += dist_v.Get(0, data.GlobalRow(row));

        }

        El::AllReduce(dist_v, El::mpi::COMM_WORLD, El::mpi::MIN);
        T recv_cuml_dist = 0;
        El::mpi::AllReduce(&cuml_dist, &recv_cuml_dist, 1,
                El::mpi::SUM, El::mpi::COMM_WORLD);
        cuml_dist = recv_cuml_dist;
        assert(cuml_dist > 0);

        cuml_dist = (cuml_dist * ((double)random())) / (RAND_MAX - 1.0);
        clust_idx++;

        for (size_t row = 0; row < data.Height(); row++) {
            cuml_dist -= dist_v.Get(0, row);

            if (cuml_dist <= 0) {
                if (data.IsLocalRow(row)) {
#if KM_DEBUG
                    El::Output("Proc: ", data.DistRank(), " assigning r: ", row,
                            " local r: ", data.LocalRow(row),
                            " as centroid: ", clust_idx);
#endif

                    centroids(El::IR(clust_idx, clust_idx+1), El::IR(0,
                                data.Width())) +=
                        local_data(El::IR(data.LocalRow(row),
                            (data.LocalRow(row)+1)), El::IR(0, data.Width()));
                }

                El::Broadcast(centroids, El::mpi::COMM_WORLD,
                        data.RowOwner(row));
                break;
            }
        }
        assert(cuml_dist <= 0);
    }
#endif
}

template <typename T>
void init_centroids(El::Matrix<T>& centroids, const El::DistMatrix<T,
        El::STAR, El::VC>& data, init_t init, const El::Unsigned seed,
        std::vector<El::Unsigned>& centroid_assignment,
        El::Matrix<El::Int>& assignment_count) {

    El::Unsigned nprocs = El::mpi::Size(El::mpi::COMM_WORLD);
    El::Unsigned dim = data.Height();
    El::Unsigned k = centroids.Width();
    El::Unsigned rank = data.DistRank();

#if KM_DEBUG
    if (rank == root)
        El::Output("nprocs: ", nprocs, ", dim: ", dim, ", k:", k,
                ", rank:", rank);
#endif

    switch (init) {
        case init_t::RANDOM: {
            // Get the local data first
            El::Matrix<double> local_data = data.LockedMatrix();
            mpi_random_generator<El::Unsigned> gen(0, k-1, rank, nprocs, seed);
            for (El::Unsigned col = 0; col < local_data.Width(); col++) {
                El::Unsigned chosen_centroid_id = gen.next();

#if KM_DEBUG
                El::Output("Point: ", data.GlobalCol(col),
                        " chose c: ", chosen_centroid_id);
#endif

                // Add sample to local clusters
                centroids(El::IR(0, dim),
                        El::IR(chosen_centroid_id, chosen_centroid_id+1)) +=
                    local_data(El::IR(0, dim), El::IR(col, col+1));

                // Increase cluster count
                assignment_count.Set(0, chosen_centroid_id,
                        assignment_count.Get(0, chosen_centroid_id) + 1);

                // Note the rows membership
                centroid_assignment[col] = chosen_centroid_id;
            }

            // Now we must merge per proc centroids
            El::AllReduce(centroids, El::mpi::COMM_WORLD, El::mpi::SUM);
            El::AllReduce(assignment_count, El::mpi::COMM_WORLD, El::mpi::SUM);

            // Get the means of the global centroids
            col_mean_raw(centroids, centroids, assignment_count);
            // Reset the assignment count
            El::Zero(assignment_count);
        }
        break;
        case init_t::FORGY: {
#if 0
            El::Zeros(centroids, k, ncol);
            El::Matrix<T> local_data = data.LockedMatrix();

            mpi_random_generator<El::Unsigned>
                gen(0, nrow-1, 0, 1, seed);

            for (El::Unsigned cl = 0; cl < k; cl++) {
                El::Unsigned chosen = gen.next(); // Globel index

                if (data.IsLocalRow(chosen)) {
                    El::Unsigned local_chosen = data.LocalRow(chosen);
#if KM_DEBUG
                    printf("Adding r: %llu as c: %llu in proc: %llu ...\n",
                            chosen, cl, rank);
#endif
                    centroids(El::IR(cl, cl+1), El::IR(0, ncol)) =
                        local_data(El::IR(local_chosen, local_chosen+1),
                                El::IR(0, ncol));
                }
            }

            // Make sure all procs have the same centroids
            El::AllReduce(centroids, El::mpi::COMM_WORLD, El::mpi::SUM);
#endif
            throw std::runtime_error("Not yet implemented");
        }
        break;
        case init_t::NONE:
            // Do Nothing
            break;
        case init_t::PLUSPLUS:
#if 0
            kmeanspp_init(data, centroids, seed, k);
#endif
            throw std::runtime_error("Not yet implemented");
            break;
        case init_t::BARBAR:
            throw std::runtime_error("Not yet implemented");
            break;
        case init_t::SKETCH:
            throw std::runtime_error("Not yet implemented");
            break;
        default:
            throw std::runtime_error("Unknown"
                    " intialization method!");
    }
}

init_t get_init_type(std::string init) {
    if (std::string("random") == init)
        return init_t::RANDOM;
    else if (std::string("forgy") == init)
        return init_t::FORGY;
    else if (std::string("plusplus") == init)
        return init_t::PLUSPLUS;
    else if (std::string("barbar") == init)
        return init_t::BARBAR;
    else if (std::string("sketch") == init)
        return init_t::SKETCH;
    else if (std::string("none") == init)
        return init_t::NONE;
    else {
        std::string err = std::string("Unknown "
                "intialization method '") + init + "'";
        throw std::runtime_error(err);
    }
}

// Get the sum of a matrix
template <typename T>
T sum(const El::Matrix<T>& mat) {
    T total = 0;
    for (size_t row = 0; row < mat.Height(); row++)
        for (size_t col = 0; col < mat.Width(); col++)
            total += mat.Get(row, col);
    return total;
}

template <typename T>
void kmeans_iteration(const El::Matrix<T>& data, El::Matrix<T>& centroids,
        El::Matrix<T>& local_centroids, El::Matrix<El::Int>& assignment_count,
        std::vector<El::Unsigned>& centroid_assignment,
        El::Unsigned& nchanged) {
    // Populate per process centroids and keep track of how many
    const El::Unsigned k = centroids.Width();
    const size_t nsamples =  data.Width();
    const size_t dim = data.Height();

#if KM_DEBUG
    if (El::mpi::Rank(El::mpi::COMM_WORLD) == 0) {
        El::Output("Process 0 has ", nsamples, " samples");
    }
#endif

    for (size_t sample = 0; sample < nsamples; sample++) {
        El::Unsigned assigned_centroid_id = INVALID_ID;
        T dist = std::numeric_limits<T>::max();
        T best = std::numeric_limits<T>::max();

        for (El::Unsigned cl = 0; cl < k; cl++) {
            dist = euclidean_distance<T>(data.LockedBuffer(0, sample),
                    centroids.LockedBuffer(0, cl), dim);
            if (dist < best) {
                best = dist;
                assigned_centroid_id = cl;
            }
        }

        assert(assigned_centroid_id != INVALID_ID);

        // Have I changed clusters ?
        if (centroid_assignment[sample] != assigned_centroid_id) {
#if KM_DEBUG
            El::Output("Sample: ", sample, " => OC: ",
                    centroid_assignment[sample],
                    " NC: ", assigned_centroid_id, "\n");
#endif
            centroid_assignment[sample] = assigned_centroid_id;
            nchanged++;
        }

        add_sample<T>(local_centroids, assigned_centroid_id,
                data, sample, assignment_count);
    }

    assert(sum(assignment_count) == nsamples);
}

template <typename T>
void kmeans_titeration(const El::Matrix<T>& data, El::Matrix<T>& centroids,
        El::Matrix<T>& local_centroids, El::Matrix<El::Int>& assignment_count,
        std::vector<El::Unsigned>& centroid_assignment,
        El::Unsigned& nchanged,
        kpmbase::thd_safe_bool_vector::ptr recalculated_v,
        std::vector<T>& dist_v, kpmprune::dist_matrix::ptr dm,
        std::vector<T>& s_val_v, std::vector<T>& prev_dist,
        const bool prune_init=false) {
    // Populate per process centroids and keep track of how many
    const El::Unsigned k = centroids.Width();
    const size_t local_nsamples =  data.Width(); // Local nsamples
    const size_t dim = data.Height();
    assert(prev_dist.size() == centroids.Width());

#if KM_DEBUG
    if (El::mpi::Rank(El::mpi::COMM_WORLD) == 0)
        El::Output("Process 0 has ", local_nsamples, " samples");
#endif

    for (size_t sample = 0; sample < local_nsamples; sample++) {
        El::Unsigned prev_centroid_id = centroid_assignment[sample];
        El::Unsigned assigned_centroid_id = INVALID_ID;

        if (prune_init) {
            El::Unsigned assigned_centroid_id = INVALID_ID;
            T dist = std::numeric_limits<T>::max();

            for (El::Unsigned cl = 0; cl < k; cl++) {
                dist = euclidean_distance<T>(data.LockedBuffer(0, sample),
                        centroids.LockedBuffer(0, cl), dim);
                if (dist < dist_v[sample]) {
                    dist_v[sample] = dist;
                    assigned_centroid_id = cl;
                }
            }
        } else {
            recalculated_v->set(sample, false);
            dist_v[sample] += prev_dist(centroid_assignment[sample]);

            if (dist_v[sample] <= s_val_v(centroid_assignment[sample])) {
                // Skip all rows
            } else {
                for (unsigned cl = 0; cl < k; cl++) {
                    if (dist_v[sample] <= dm->get(
                                centroid_assignment[sample], cl))
                        continue; // Skip this cluster

                    if (!recalculated_v->get(sample)) {
                        dist_v[sample] = euclidean_distance<T>(
                                data.LockedBuffer(0, sample),
                                centroids.LockedBuffer(0,
                                    centroid_assignment[sample]), dim);
                        recalculated_v->set(sample, true);
                    }

                    if (dist_v[sample] <= dm->get(
                                centroid_assignment[sample], cl))
                        continue; // Skip this cluster

                    // Track 5
                    T jdist = euclidean_distance<T>(
                            data.LockedBuffer(0, sample),
                            centroids.LockedBuffer(0, cl), dim);

                    if (jdist < dist_v[sample]) {
                        dist_v[sample] = jdist;
                        centroid_assignment[sample] = cl;
                    }
                } // endfor
            }
        }

        assert(assigned_centroid_id != INVALID_ID);
        centroid_assignment[sample] = assigned_centroid_id;

        if (prune_init) {
            nchanged++;
            // NOTE: local_centroids are zerod
            add_sample<T>(local_centroids, assigned_centroid_id,
                    data, sample, assignment_count);
        } else if (assigned_centroid_id != prev_centroid_id) {
            nchanged++;
            remove_sample<T>(local_centroids, prev_centroid_id,
                    data, sample, assignment_count);
            add_sample<T>(local_centroids, assigned_centroid_id,
                    data, sample, assignment_count);
        }
    }

    assert(sum(assignment_count) == local_nsamples);
}
/**
  * An inefficient way to gather the assignment of every point and
  *     interleave them into a single vector.
  */
void get_global_assignments(const std::vector<El::Unsigned>&
        centroid_assignment, std::vector<El::Unsigned>&
        gl_centroid_assignments) {
    El::mpi::Comm comm = El::mpi::COMM_WORLD;
    El::Unsigned nprocs = El::mpi::Size(comm);
    El::Unsigned rank = El::mpi::Rank(comm);

    El::Matrix<El::Int> samples_per_proc; // samples per process
    El::Zeros(samples_per_proc, 1, nprocs);
    // Each proc assigns only its nsamples
    samples_per_proc.Set(0, rank, centroid_assignment.size());
    El::AllReduce(samples_per_proc, comm, El::mpi::SUM);

#if KM_DEBUG
    if (rank == root)
        El::Print(samples_per_proc, "\nSamples per process: ");
#endif

    std::vector<std::vector<El::Unsigned> > all_centroid_assignment(nprocs);
    if (rank != root) {
        El::mpi::Send(&centroid_assignment[0], (int)centroid_assignment.size(),
                root, comm);
    }
    else {
        for (El::Unsigned p = 0; p < nprocs; p++)
            all_centroid_assignment[p].resize(samples_per_proc.Get(0, p));

        // Copy the root for ease of accumulation
        all_centroid_assignment[root] = centroid_assignment;
        for (El::Unsigned srank = 1; srank < nprocs; srank++)
            El::mpi::Recv(&((all_centroid_assignment[srank])[0]),
                    samples_per_proc.Get(0, srank), srank, comm);

        // Cache UNfriendly access pattern here
        for (size_t memb = 0; memb <
                all_centroid_assignment[0].size(); memb++) {
            for (El::Unsigned p = 0; p < nprocs; p++) {
                // Some may have fewer
                if (memb < all_centroid_assignment[p].size())
                    gl_centroid_assignments.
                        push_back(all_centroid_assignment[p][memb]);
            }
        }
    }
}
}

namespace skylark { namespace ml {
    /**
      * Type used to return items from the computation of
      *     kmeans.
      */
template <typename T>
class kmeans_t {
public:
    std::vector<El::Unsigned> gl_centroid_assignments;
    std::vector<El::Int> assignment_count;
    El::Unsigned iters;
    std::vector<T> centroids;

    kmeans_t(std::vector<El::Unsigned>& gl_centroid_assignments,
            El::Int* assignment_count_buf, const size_t k,
            const El::Unsigned iters, El::Matrix<T>& centroids) {

        this->gl_centroid_assignments = gl_centroid_assignments;
        this->iters = iters;
        this->assignment_count.resize(k);
        std::copy(assignment_count_buf, assignment_count_buf + k,
                assignment_count.begin());

        // Get centroids row major
            for(El::Unsigned col = 0; col < centroids.Width();
                    col++) {
                for (El::Unsigned row = 0; row < centroids.Height();
                        row++) {
                    this->centroids.push_back(centroids.Get(row, col));
                }
            }
    }

    void print() {
        El::Output("Iterations: ", iters);
        El::Output("Cluster count: ");
        skyutil::pretty_printer<El::Int>::print_vector(assignment_count);
    }
};

/**
  * Driver for the kmeans algorithm.
  *
  * @param data: A distributed matrix with each column representing a single
  *     vertex/data point
  * @param centroids: The cluster centroids or centers.
  * @param k: The number of clusters.
  * @param tol: The tolerance is the minimum percent change of membership
        from iteration i to i+1 such that we consider kmeans to have converged.
  * @param init: The type of initialization to use [forgy | random | plusplus]
  * @param seed: A seed to the pseudorandom number generator used.
  * @param max_iters: The maximum number of iterations kmeans can perform.
  * @param rank: The number of parallel processes running.
  */
template<typename T>
kmeans_t<T> run_kmeans(El::DistMatrix<T, El::STAR, El::VC>& data,
        El::Matrix<T>& centroids, const El::Unsigned k,
        const double tol, const std::string init,
        const El::Int seed, const El::Unsigned max_iters) {
    El::Unsigned nchanged = 0;

    El::Unsigned nsamples = data.Width(); // Global nsamples
    El::Unsigned nlocal_samples = data.LocalWidth();
    El::Unsigned rank = data.DistRank();
    El::Unsigned dim = data.Height();

    clock_t t = clock();

    // Count #samples in each centroid per proc
    El::Matrix<El::Int> assignment_count(1, k);
    El::Zero(assignment_count);

    El::Matrix<T> local_centroids(dim, k);
    El::Zero(local_centroids);

    std::vector<El::Unsigned> centroid_assignment;
    centroid_assignment.assign(nlocal_samples, INVALID_ID);

    init_centroids<T>(centroids, data, get_init_type(init),
            seed, centroid_assignment, assignment_count);

    // Run iterations
    double perc_changed = std::numeric_limits<double>::max();
    El::Unsigned iters = 0;
    bool converged = false;

    El::mpi::Comm comm = El::mpi::COMM_WORLD;

    while (perc_changed > tol && iters < max_iters) {
        El::Zero(assignment_count); // Reset

        if (rank == root)
            El::Output("Running iteration ", iters, " ...\n");

        kmeans_iteration<T>(data.LockedMatrix(), centroids, local_centroids,
                assignment_count, centroid_assignment, nchanged);
        iters++;

        El::Unsigned recv_nchanged = INVALID_ID;
        El::mpi::AllReduce(&nchanged, &recv_nchanged, 1, El::mpi::SUM, comm);
        assert(recv_nchanged != INVALID_ID);
        nchanged = recv_nchanged;

        El::AllReduce(assignment_count, comm, El::mpi::SUM);

        if (rank == root)
            El::Output("Global nchanged: ", nchanged);

        perc_changed = (double)nchanged/nsamples; //Global perc change
        if (perc_changed <= tol) {
            converged = true;
            if (rank == root) {
                El::Output("Algorithm converged in ", iters,
                        " iterations!");
            }
            break;
        }

#if KM_DEBUG
        if (rank == root) El::Output("Reducing local centroids ...\n");
#endif

        // Aggregate all local centroids
        El::AllReduce(local_centroids, comm, El::mpi::SUM);
        // Get the means of the local centroids
        col_mean_raw(local_centroids, centroids, assignment_count);

#if KM_DEBUG
        if (rank == root)
            El::Print(centroids, "Updated centroids for root");
#endif

        // Reset
        nchanged = 0;
        El::Zero(local_centroids);
    }

    // Get the centroid assignments to the root
    std::vector<El::Unsigned> gl_centroid_assignments;
    get_global_assignments(centroid_assignment, gl_centroid_assignments);

#if 1
    if (rank == root) {
        El::Print(assignment_count, "\nFinal assingment count");
        El::Output("Centroid assignment:");
        skyutil::pretty_printer<El::Unsigned>::
            print_vector(gl_centroid_assignments);
    }

    if (!converged && rank == root)
        El::Output("Algorithm failed to converge in ",
                iters, " iterations\n");
#endif

    t = clock() - t;
    if (rank == root)
        El::Output("\nK-means took ",((float)t)/CLOCKS_PER_SEC, " sec ...");

    return kmeans_t<T>(gl_centroid_assignments,
            assignment_count.Buffer(), k, iters, centroids);
}

template<typename T>
kmeans_t<T> run_tri_kmeans(El::DistMatrix<T, El::STAR, El::VC>& data,
        El::Matrix<T>& centroids, const El::Unsigned k,
        const double tol, const std::string init,
        const El::Int seed, const El::Unsigned max_iters) {

    // Var init
    El::Unsigned nchanged = 0;
    El::Unsigned nsamples = data.Width(); // Global nsamples
    El::Unsigned nlocal_samples = data.LocalWidth();
    El::Unsigned rank = data.DistRank();
    El::Unsigned dim = data.Height();
    El::mpi::Comm comm = El::mpi::COMM_WORLD;
    clock_t t = clock();

    // Count #samples in each centroid per proc
    El::Matrix<El::Int> assignment_count(1, k);
    El::Zero(assignment_count);

    El::Matrix<T> local_centroids(dim, k);
    El::Zero(local_centroids);

    std::vector<El::Unsigned> centroid_assignment;
    centroid_assignment.assign(nlocal_samples, INVALID_ID);

    double perc_changed = std::numeric_limits<double>::max();
    bool converged = false;
    El::Unsigned iters = 0;

    // For pruning
    kpmbase::thd_safe_bool_vector::ptr recalculated_v =
        kpmbase::thd_safe_bool_vector::create(nsamples, false);
    kpmprune::dist_matrix::ptr dm = kpmprune::dist_matrix::create(k);

    std::vector<T> dist_v;
    dist_v.assign(nsamples, std::numeric_limits<T>::max());

    El::Matrix<T> prev_centroids;
    El::Matrix<El::Int> prev_assignment_count;

    // Initialize algo
    init_centroids<T>(centroids, data, get_init_type(init),
            seed, centroid_assignment, assignment_count);
    std::vector<T> prev_dist(centroids.Width());
    // FIXME: NONE init dm->compute_dist(...)

    std::vector<T> s_val_v;
    s_val_v.assign(centroids.Width(), std::numeric_limits<T>::max());

#if KM_DEBUG
    dm->compute_dist(centroids, s_val_v);
    El::Output("Cluster distance matrix after init ...");
    dm->print();
#endif

    while (perc_changed > tol && iters < max_iters) {
        El::Zero(assignment_count); // Reset

        if (rank == root)
            El::Output("Running iteration ", iters, " ...\n");

        if (iters == 0) {
            kmeans_titeration<T>(data.LockedMatrix(), centroids,
                    local_centroids, assignment_count, centroid_assignment,
                    nchanged, recalculated_v, dist_v, dm,
                    s_val_v, prev_dist, true);
        } else {
            dm->compute_dist(centroids, s_val_v);
            kmeans_titeration<T>(data.LockedMatrix(), centroids,
                    local_centroids, assignment_count, centroid_assignment,
                    nchanged, recalculated_v, dist_v, s_val_v, dm, prev_dist);
        }

        El::Unsigned recv_nchanged = INVALID_ID;
        El::mpi::AllReduce(&nchanged, &recv_nchanged, 1, El::mpi::SUM, comm);
        assert(recv_nchanged != INVALID_ID);
        nchanged = recv_nchanged;

        El::AllReduce(assignment_count, comm, El::mpi::SUM);

        if (rank == root)
            El::Output("Global nchanged: ", nchanged);

        perc_changed = (double)nchanged/nsamples; // Global perc change
        if (perc_changed <= tol) {
            converged = true;
            if (rank == root) {
                El::Output("Algorithm converged in ", (iters + 1),
                        " iterations!");
            }
            break;
        }

#if KM_DEBUG
        if (rank == root) El::Output("Reducing local centroids ...\n");
#endif

        // Aggregate all local centroids
        El::AllReduce(local_centroids, comm, El::mpi::SUM);
        prev_centroids = centroids;

        if (iters == 0)
            El::Zero(centroids);
        else
            unmean(centroids, prev_assignment_count);

        centroids += local_centroids;
        // Get the means of the local centroids
        col_mean_raw(centroids, centroids, assignment_count);
        // Copy assignment_count
        prev_assignment_count = assignment_count;
        // Get dist from prev
        get_prev_dist(prev_centroids, centroids, prev_dist);

#if KM_DEBUG
        if (rank == root)
            El::Print(centroids, "Updated centroids for root");
#endif

        // Reset
        nchanged = 0;
        El::Zero(local_centroids);
        iters++;
    }

    // Get the centroid assignments to the root
    std::vector<El::Unsigned> gl_centroid_assignments;
    get_global_assignments(centroid_assignment, gl_centroid_assignments);

#if 1
    if (rank == root) {
        El::Print(assignment_count, "\nFinal assingment count");
        El::Output("Centroid assignment:");
        skyutil::pretty_printer<El::Unsigned>::
            print_vector(gl_centroid_assignments);
    }

    if (!converged && rank == root)
        El::Output("Algorithm failed to converge in ",
                iters, " iterations\n");
#endif

    t = clock() - t;
    if (rank == root)
        El::Output("\nK-means took ",((float)t)/CLOCKS_PER_SEC, " sec ...");

    return kmeans_t<T>(gl_centroid_assignments,
            assignment_count.Buffer(), k, iters, centroids);
}

} } // namespace skylark::ml
#endif /* SKYLARK_KMEANS_HPP */
