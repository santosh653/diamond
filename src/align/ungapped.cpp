/****
DIAMOND protein aligner
Copyright (C) 2020 Max Planck Society for the Advancement of Science e.V.

Code developed by Benjamin Buchfink <benjamin.buchfink@tue.mpg.de>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
****/

#include <array>
#include <algorithm>
#include <limits.h>
#include <utility>
#include <atomic>
#include <mutex>
#include <limits.h>
#include "../basic/config.h"
#include "../stats/hauser_correction.h"
#include "target.h"
#include "../dp/ungapped.h"
#include "target.h"
#include "../data/reference.h"
#include "../util/log_stream.h"
#include "../util/parallel/thread_pool.h"
#include "../chaining/chaining.h"
#include "../dp/dp.h"

using std::array;
using std::vector;
using std::list;
using std::atomic;
using std::mutex;

namespace Extension {

std::vector<int16_t*> target_matrices;
std::mutex target_matrices_lock;
atomic<size_t> target_matrix_count(0);

WorkTarget ungapped_stage(SeedHit *begin, SeedHit *end, const sequence *query_seq, const Bias_correction *query_cb, const double* query_comp, const int16_t** query_matrix, uint32_t block_id, Statistics& stat) {
	array<vector<Diagonal_segment>, MAX_CONTEXT> diagonal_segments;
	task_timer timer;
	WorkTarget target(block_id, ref_seqs::get()[block_id], (int)query_seq[0].length(), query_comp, query_matrix);
	stat.inc(Statistics::TIME_MATRIX_ADJUST, timer.microseconds());
	if (!Stats::CBS::avg_matrix(config.comp_based_stats) && target.adjusted_matrix())
		stat.inc(Statistics::MATRIX_ADJUST_COUNT);

	if (config.ext == "full") {
		for (const SeedHit *hit = begin; hit < end; ++hit)
			target.ungapped_score = std::max(target.ungapped_score, hit->score);
		return target;
	}
	std::sort(begin, end);
	for (const SeedHit *hit = begin; hit < end; ++hit) {
		target.ungapped_score = std::max(target.ungapped_score, hit->score);
		if (!diagonal_segments[hit->frame].empty() && diagonal_segments[hit->frame].back().diag() == hit->diag() && diagonal_segments[hit->frame].back().subject_end() >= hit->j)
			continue;
		const auto d = xdrop_ungapped(query_seq[hit->frame], target.seq, hit->i, hit->j);
		if (d.score > 0) {
			diagonal_segments[hit->frame].push_back(d);
		}
	}
	for (unsigned frame = 0; frame < align_mode.query_contexts; ++frame) {
		if (diagonal_segments[frame].empty())
			continue;
		std::stable_sort(diagonal_segments[frame].begin(), diagonal_segments[frame].end(), Diagonal_segment::cmp_diag);
		pair<int, list<Hsp_traits>> hsp = greedy_align(query_seq[frame], target.seq, diagonal_segments[frame].begin(), diagonal_segments[frame].end(), config.log_extend, frame);
		target.hsp[frame] = std::move(hsp.second);
		target.hsp[frame].sort(Hsp_traits::cmp_diag);
	}
	return target;
}

void ungapped_stage_worker(size_t i, size_t thread_id, const sequence *query_seq, const Bias_correction *query_cb, const double* query_comp, FlatArray<SeedHit> *seed_hits, const uint32_t*target_block_ids, vector<WorkTarget> *out, mutex *mtx, Statistics* stat) {
	Statistics stats;
	const int16_t* query_matrix = nullptr;
	WorkTarget target = ungapped_stage(seed_hits->begin(i), seed_hits->end(i), query_seq, query_cb, query_comp, &query_matrix, target_block_ids[i], stats);
	{
		std::lock_guard<mutex> guard(*mtx);
		out->push_back(std::move(target));
		*stat += stats;
	}
	delete[] query_matrix;
}

vector<WorkTarget> ungapped_stage(const sequence *query_seq, const Bias_correction *query_cb, const double* query_comp, FlatArray<SeedHit> &seed_hits, const vector<uint32_t>& target_block_ids, int flags, Statistics& stat) {
	vector<WorkTarget> targets;
	if (target_block_ids.size() == 0)
		return targets;
	const int16_t* query_matrix = nullptr;
	if (flags & DP::PARALLEL) {
		mutex mtx;
		Util::Parallel::scheduled_thread_pool_auto(config.threads_, seed_hits.size(), ungapped_stage_worker, query_seq, query_cb, query_comp, &seed_hits, target_block_ids.data(), &targets, &mtx, &stat);
	}
	else {
		for (size_t i = 0; i < target_block_ids.size(); ++i)
			targets.push_back(ungapped_stage(seed_hits.begin(i), seed_hits.end(i), query_seq, query_cb, query_comp, &query_matrix, target_block_ids[i], stat));
	}

	delete[] query_matrix;
	return targets;
}

}