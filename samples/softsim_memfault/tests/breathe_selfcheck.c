/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026 Onomondo ApS
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Host self-check for the LED breathe ramp in ../src/main.c. Not part of the
 * firmware build - the ramp math is the only off-by-one-prone logic, so it is
 * factored into a pure breathe_pct() and asserted here. Keep this copy in sync
 * with src/main.c (BREATHE_STEPS and breathe_pct must match).
 *
 *   cc -o /tmp/breathe_selfcheck tests/breathe_selfcheck.c && /tmp/breathe_selfcheck
 */

#include <assert.h>
#include <stdio.h>

#define BREATHE_STEPS 20

static int breathe_pct(int step)
{
	const int half = BREATHE_STEPS / 2;

	if (step < half) {
		return (step + 1) * 100 / half; /* ramp up: 10..100 */
	}
	if (step < BREATHE_STEPS) {
		return (BREATHE_STEPS - 1 - step) * 100 / half; /* ramp down: 90..0 */
	}
	return -1; /* sequence complete */
}

int main(void)
{
	const int half = BREATHE_STEPS / 2;

	/* First step already visible (not 0), and the sequence terminates. */
	assert(breathe_pct(0) > 0);
	assert(breathe_pct(BREATHE_STEPS) < 0);
	assert(breathe_pct(BREATHE_STEPS + 5) < 0);

	int prev = 0;

	/* Ramp up: strictly non-decreasing, within 0..100, peaks at 100. */
	for (int s = 0; s < half; s++) {
		int p = breathe_pct(s);

		assert(p >= 0 && p <= 100);
		assert(p >= prev);
		prev = p;
	}
	assert(breathe_pct(half - 1) == 100); /* reaches full brightness */

	/* Ramp down: strictly non-increasing back toward off, within 0..100. */
	prev = 100;
	for (int s = half; s < BREATHE_STEPS; s++) {
		int p = breathe_pct(s);

		assert(p >= 0 && p <= 100);
		assert(p <= prev);
		prev = p;
	}
	assert(breathe_pct(BREATHE_STEPS - 1) == 0); /* ends dark */

	printf("breathe_selfcheck: OK (%d steps, peak 100, ends 0)\n", BREATHE_STEPS);
	return 0;
}
