#ifndef SDD_CG_HPP
#define SDD_CG_HPP

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cmath>

#include <boost/range/algorithm.hpp>

#include <amgcl/amgcl.hpp>
#include <amgcl/backend/builtin.hpp>
#include <amgcl/backend/crs_tuple.hpp>
#include <amgcl/coarsening/plain_aggregates.hpp>
#include <amgcl/coarsening/smoothed_aggregation.hpp>
#include <amgcl/relaxation/spai0.hpp>
#include <amgcl/solver/cg.hpp>
#include <amgcl/mpi/deflation.hpp>
#include <amgcl/profiler.hpp>

#include "domain_partition.hpp"

struct linear_deflation {
    long n;
    double h;
    std::vector<long> idx;

    linear_deflation(long n) : n(n), h(1.0 / (n - 1)), idx(n * n) {}

    size_t dim() const { return 3; }

    double operator()(long i, int j) const {
        switch(j) {
            case 1:
                return h * (idx[i] % n);
            case 2:
                return h * (idx[i] / n);
            case 0:
            default:
                return 1;
        }
    }
};

int main(int argc, char *argv[]) {
    boost::mpi::environment  env;
    boost::mpi::communicator world;

    const long n  = argc > 1 ? atoi(argv[1]) : 1024;
    const long n2 = n * n;

    boost::array<long, 2> lo = { {0, 0} };
    boost::array<long, 2> hi = { {n - 1, n - 1} };

    amgcl::profiler<> prof;

    prof.tic("partition");
    domain_partition<2> part(lo, hi, world.size());
    const long chunk = part.size( world.rank() );

    std::vector<long> domain(world.size() + 1);
    all_gather(world, chunk, &domain[1]);
    boost::partial_sum(domain, domain.begin());

    const long chunk_start = domain[world.rank()];
    const long chunk_end   = domain[world.rank() + 1];

    linear_deflation lindef(n);
    std::vector<long> renum(n2);
    for(long j = 0, idx = 0; j < n; ++j) {
        for(long i = 0; i < n; ++i, ++idx) {
            boost::array<long, 2> p = {{i, j}};
            std::pair<int,long> v = part.index(p);
            renum[idx] = domain[v.first] + v.second;
            lindef.idx[renum[idx]] = idx;
        }
    }
    prof.toc("partition");

    prof.tic("assemble");
    std::vector<long>   ptr;
    std::vector<long>   col;
    std::vector<double> val;
    std::vector<double> rhs;

    ptr.reserve(chunk + 1);
    col.reserve(chunk * 5);
    val.reserve(chunk * 5);
    rhs.reserve(chunk);

    ptr.push_back(0);

    const double h2i = (n - 1) * (n - 1);
    for(long j = 0, idx = 0; j < n; ++j) {
        for(long i = 0; i < n; ++i, ++idx) {
            if (renum[idx] < chunk_start || renum[idx] >= chunk_end) continue;

            if (j > 0)  {
                col.push_back(renum[idx - n]);
                val.push_back(-h2i);
            }

            if (i > 0) {
                col.push_back(renum[idx - 1]);
                val.push_back(-h2i);
            }

            col.push_back(renum[idx]);
            val.push_back(4 * h2i);

            if (i + 1 < n) {
                col.push_back(renum[idx + 1]);
                val.push_back(-h2i);
            }

            if (j + 1 < n) {
                col.push_back(renum[idx + n]);
                val.push_back(-h2i);
            }

            rhs.push_back(1);
            ptr.push_back( col.size() );
        }
    }
    prof.toc("assemble");

    prof.tic("setup");
    typedef amgcl::mpi::subdomain_deflation<
        amgcl::backend::builtin<double>,
        amgcl::coarsening::smoothed_aggregation<
            amgcl::coarsening::plain_aggregates
            >,
        amgcl::relaxation::spai0,
        amgcl::solver::cg
        > Solver;

    typename Solver::AMG_params    amg_prm;
    typename Solver::Solver_params slv_prm(500, 1e-6);
    Solver solve(world,
            boost::tie(chunk, ptr, col, val),
            lindef,
            amg_prm, slv_prm
            );
    prof.toc("setup");

    prof.tic("solve");
    std::vector<double> x(chunk, 0);
    size_t iters;
    double resid;
    boost::tie(iters, resid) = solve(rhs, x);
    prof.toc("solve");

    prof.tic("save");
    if (world.rank() == 0) {
        std::vector<double> X(n2);
        boost::copy(x, X.begin());

        for(int i = 1; i < world.size(); ++i)
            world.recv(i, 42, &X[domain[i]], domain[i+1] - domain[i]);

        std::ofstream f("out.dat", std::ios::binary);
        int m = n2;
        f.write((char*)&m, sizeof(int));
        for(long i = 0; i < n2; ++i)
            f.write((char*)&X[renum[i]], sizeof(double));
    } else {
        world.send(0, 42, x.data(), chunk);
    }
    prof.toc("save");

    if (world.rank() == 0) {
        std::cout
            << "Iterations: " << iters << std::endl
            << "Error:      " << resid << std::endl
            << std::endl
            << prof << std::endl;
    }
}

#endif