#ifndef PARTICLE_COMPRESS_H
#define PARTICLE_COMPRESS_H

/**
 * @brief This function takes k_particle_movers as a map to tell us where gaps
 * will be in the array, and fills those gaps in parallel
 *
 * This assumes the movers will not have any repeats in indexing, otherwise
 * it's not safe to do in parallel
 *
 * @param k_particles The array to compact
 * @param k_particle_movers The array holding the packing mask
 * @param nm Num movers
 * @param np Num particles
 * @param sp Species operating on
 */
struct DefaultCompress {
    static void compress(
            k_particles_t particles,
            k_particles_i_t particles_i,
            k_particle_i_movers_t particle_movers_i,
            const int32_t nm,
            const int32_t np,
            species_t* sp
            )
    {
        // From particle_movers(nm, particle_mover_var::pmi), we know where the
        // gaps in particle are

        // We can safely back fill the gaps in parallel by looping over the movers,
        // which are guaranteed to be unique, and sending np-i to that index

        // WARNING: In SoA configuration this may get a bit cache thrashy?
        // TODO: this may perform better if the index in the movers are sorted first..

        // This is a O(NP) solution. There likely exists a faster O(NM) solution
        // but my attempt had a data race

        // POSSIBLE IMPROVEMENT, A better way to do is this?:
        //   Run the back fill loop but if a "pull_from" id is a gap (which can be
        //   detected by setting a special flag in it's p->i value), then skip it.
        //   Instead add the index of that guy to an (atomic) clean up list
        //
        //   Do a second pass over the cleanup list ? (care of repeated data race..)

        // This is a little slow, but not a deal breaker
        // Build a list of "safe" filling ids, to avoid data race
        // we do this for the case where a "gap" exists in the backfill region (np-nm)

        // TODO: prevent this malloc every time
        // Track is the last 2*nm particles in np are "unsafe" to pull from (i.e
        // are "gaps")
        // We want unsafe_index to map up in reverse
        // [ 0  , 1   , 2   , 3... 2nm ] is equal to
        // [np-1, np-2, np-3... np-1-2nm]
        // i.e 0 is the "last particle"
        // This is annoying, but it will give a back fill order more consistent
        // with VPIC's serial algorithm

        Kokkos::View<int*> unsafe_index("safe index", 2*nm);

        // TODO: prevent these allocations from being repeated.

        // Track (atomically) the id's we've tried to pull from when dealing with a
        // gap in the "danger zone"
        Kokkos::View<int> panic_counter("panic counter");

        // We use this to store a list of things we bailed out on moving. Typically because the mapping of pull_from->write_to got skipped.

        Kokkos::View<int> clean_up_to_count("clean up to count"); // todo: find an algorithm that doesn't need this
        Kokkos::View<int> clean_up_from_count("clean up from count"); // todo: find an algorithm that doesn't need this

        Kokkos::View<int>::HostMirror clean_up_to_count_h = Kokkos::create_mirror_view(clean_up_to_count);
        Kokkos::View<int>::HostMirror clean_up_from_count_h = Kokkos::create_mirror_view(clean_up_from_count);

        Kokkos::View<int*> clean_up_from("clean up from", nm);
        Kokkos::View<int*> clean_up_to("clean up to", nm);

        // Loop over 2*nm, which is enough to guarantee you `nm` non-gaps
        // Build a list of safe lookups

        // TODO: we can probably do this online while we do the advance_p
        Kokkos::parallel_for("particle compress", Kokkos::RangePolicy <
        Kokkos::DefaultExecutionSpace > (0, nm), KOKKOS_LAMBDA (int i)
        {
            // If the id of this particle is in the danger zone, don't add it
            // otherwise, do
            int cut_off = np-(2*nm);

            int pmi = particle_movers_i(i);

            // If it's less than the cut off, it's safe
            if ( pmi >= cut_off) // danger zone
            {
                int index = ((np-1) - pmi); // Map to the reverse indexing
                unsafe_index(index) = 1; // 1 marks it as unsafe
            }
        });

        // We will use the first 0-nm of safe_index to pull from
        // We will use the nm -> 2nm range for "panic picks", if the first wasn't safe (involves atomics..)
        Kokkos::parallel_for("particle compress", Kokkos::RangePolicy <
                Kokkos::DefaultExecutionSpace > (0, nm), KOKKOS_LAMBDA (int n)
        {

            // TODO: is this np or np-1?
            // Doing this in the "inverse order" to match vpic
            int pull_from = (np-1) - (n); // grab a particle from the end block
            int write_to = particle_movers_i(nm-n-1); // put it in a gap
            int danger_zone = np - nm;

            // if they're the same, no need to do it. This can happen below in the
            // danger zone and we want to avoid "cleaning it up"
            if (pull_from == write_to) return;

            // If the "gap" is in the danger zone, no need to back fill it
            if (write_to >= danger_zone)
            {
            // if pull from is unsafe, skip it
            if (pull_from >= danger_zone) // We only have lookup for the danger zone
            {
            if ( ! unsafe_index( (np-1) - pull_from ) )
            {
                // FIXME: if it's not in the danger zone, someone else will
                // fill it..but then we don't want to move it??? is this
                // true, and a very subtle race condition?

                // TODO: by skipping this move, we neglect to move the
                // pull_from to somewhere sensible...  For now we put it on
                // a clean up list..but that sucks
                int clean_up_from_index = Kokkos::atomic_fetch_add( &clean_up_from_count(), 1 );
                clean_up_from(clean_up_from_index) = pull_from;
            }
            }

            return;
            }

            //int safe_index_offset = (np-nm);

            // Detect if the index we want to pull from is safe
            // Want to index this 0...nm
            //if ( unsafe_index(pull_from - safe_index_offset ) )
            if ( unsafe_index( n ) )
            {
                // Instead we'll get this on the second pass
                int clean_up_to_index = Kokkos::atomic_fetch_add( &clean_up_to_count(), 1 );
                clean_up_to(clean_up_to_index) = write_to;

                return;
            }
            else {
                //printf("%d is safe %d\n", n, pull_from);
            }

            //printf("moving id %d %f %f %f to %d\n",
            //pull_from,
            //particles(pull_from, particle_var::dx),
            //particles(pull_from, particle_var::dy),
            //particles(pull_from, particle_var::dz),
            //write_to);

            // Move the particle from np-n to pm->i
            particles(write_to, particle_var::dx) = particles(pull_from, particle_var::dx);
            particles(write_to, particle_var::dy) = particles(pull_from, particle_var::dy);
            particles(write_to, particle_var::dz) = particles(pull_from, particle_var::dz);
            particles(write_to, particle_var::ux) = particles(pull_from, particle_var::ux);
            particles(write_to, particle_var::uy) = particles(pull_from, particle_var::uy);
            particles(write_to, particle_var::uz) = particles(pull_from, particle_var::uz);
            particles(write_to, particle_var::w)  = particles(pull_from, particle_var::w);
            particles_i(write_to) = particles_i(pull_from);
        });

        Kokkos::deep_copy(clean_up_from_count_h, clean_up_from_count);
        Kokkos::deep_copy(clean_up_to_count_h, clean_up_to_count);

        Kokkos::parallel_for("compress clean up", Kokkos::RangePolicy <
        Kokkos::DefaultExecutionSpace > (0, clean_up_from_count_h() ), KOKKOS_LAMBDA (int n)
        {
            int write_to = clean_up_to(n);
            int pull_from = clean_up_from(n);

            particles(write_to, particle_var::dx) = particles(pull_from, particle_var::dx);
            particles(write_to, particle_var::dy) = particles(pull_from, particle_var::dy);
            particles(write_to, particle_var::dz) = particles(pull_from, particle_var::dz);
            particles(write_to, particle_var::ux) = particles(pull_from, particle_var::ux);
            particles(write_to, particle_var::uy) = particles(pull_from, particle_var::uy);
            particles(write_to, particle_var::uz) = particles(pull_from, particle_var::uz);
            particles(write_to, particle_var::w)  = particles(pull_from, particle_var::w);
            particles_i(write_to) = particles_i(pull_from);
        });
    }

};

/*
struct SortCompress {
    static void compress(
            k_particles_t particles,
            k_particle_movers_t particle_movers,
            const int32_t nm,
            const int32_t np,
            species_t* sp
            )
    {
    }

};
*/

template <typename Policy = DefaultCompress>
struct ParticleCompressor : private Policy {
    using Policy::compress;
};

#endif //guard