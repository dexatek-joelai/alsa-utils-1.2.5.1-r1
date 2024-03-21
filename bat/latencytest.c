/*
 * Copyright (C) 2013-2015 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "common.h"
#include "bat-signal.h"
#include "gettext.h"

/* How one measurement step works:
   - Listen and measure the average loudness of the environment for 1 second.
   - Create a threshold value 16 decibels higher than the average loudness.
   - Begin playing a ~1000 Hz sine wave and start counting the samples elapsed.
   - Stop counting and playing if the input's loudness is higher than the
     threshold, as the output wave is probably coming back.
   - Calculate the round trip audio latency value in milliseconds. */

static float sumaudio(struct bat *bat, short int *buffer, int frames)
{
	float sum = 0;
	int n = 0;

	while (frames) {
		frames--;

		for (n = 0; n < bat->channels; n++) {
			sum += abs(buffer[0]);
			buffer++;
		}
	}

	sum = sum / bat->channels;

	return sum;
}

void _dump_test_db_num(struct bat *bat, int max) {
	int i;

	if (max < 0) {
		// latest count
		i = abs(max);
		if (i >= bat->latency.test_db_num) {
			i = 0;
		} else {
			i = bat->latency.test_db_num - i;
		}
		max = bat->latency.test_db_num;
	} else {
		i = 0;
		if (max > bat->latency.test_db_num) max = bat->latency.test_db_num;
	}
	printf("dump test db [%d..%d]", i, max - 1);
	for ( ; i < max; i++) {
		printf("%s%f", (i == 0 ? ": " : ", "), bat->latency.test_db[i]);
	}
	printf("\n");
}

static void play_and_listen(struct bat *bat, void *buffer, int frames)
{
	int averageinput;
	int n = 0;
	float sum = 0;
	float max = 0;
	float min = 100000.0f;
	short int *input;
	int num = bat->latency.number;
	int facc_pos = bat->capture.facc - bat->latency.samples;
	int facc_sz = bat->latency.samples + frames;

	averageinput = (int) (sumaudio(bat, buffer, frames) / frames);

	if (bat->latency.test_db_num < LATENCY_TEST_DB_NUMBER) {
		bat->latency.test_db[bat->latency.test_db_num] =
				20.0f * log10f((float)averageinput / 32767.0f);
		bat->latency.test_db_num++;
	}

	/* The signal is above threshold
	   So our sine wave comes back on the input */
	if (averageinput > bat->latency.threshold) {
		input = buffer;

		/* Check the location when it became loud enough */
		while (n < frames) {
			if (*input++ > bat->latency.threshold)
				break;
			*input += bat->channels;
			n++;
		}

		/* Now we get the total round trip latency*/
		bat->latency.samples += n;

		/* Expect at least 1 buffer of round trip latency. */
		if (bat->latency.samples > frames) {
			bat->latency.result[num - 1] =
				(float) bat->latency.samples * 1000 / bat->rate;
			fprintf(bat->log,
					 _("Test%d, round trip latency %dms\n"),
					num,
					(int) bat->latency.result[num - 1]);

			for (n = 0; n < num; n++) {
				if (bat->latency.result[n] > max)
					max = bat->latency.result[n];
				if (bat->latency.result[n] < min)
					min = bat->latency.result[n];
				sum += bat->latency.result[n];
			}

			/* The maximum is higher than the minimum's double */
			if (max / min > 2.0f) {
				bat->latency.state =
					LATENCY_STATE_COMPLETE_FAILURE;
				bat->latency.is_capturing = false;
				log_tdm2("Test%d %d+%d(%d) -> LATENCY_STATE_COMPLETE_FAILURE"
						", max %f / min %f > 2\n",
						num, facc_pos, facc_sz, bat->latency.samples,
						max, min);
				return;

			/* Final results */
			} else if (num == LATENCY_TEST_NUMBER) {
				bat->latency.final_result =
					(int) (sum / LATENCY_TEST_NUMBER);
				fprintf(bat->log,
					_("Final round trip latency: %dms\n"),
					bat->latency.final_result);

				bat->latency.state =
					LATENCY_STATE_COMPLETE_SUCCESS;
				bat->latency.is_capturing = false;
				log_tdm2("Test%d %d+%d(%d) -> LATENCY_STATE_COMPLETE_SUCCESS\n",
						num, facc_pos, facc_sz, bat->latency.samples);
				return;

			/* Next step */
			} else {
				bat->latency.state = LATENCY_STATE_WAITING;
				bat->latency.wait_samples -= (facc_sz + bat->period_size * 10);
				log_tdm2("Test%d %d+%d(%d) -> LATENCY_STATE_WAITING"
						", wait_samples: %d\n",
						num, facc_pos, facc_sz, bat->latency.samples,
						bat->latency.wait_samples);
				dump_test_db_num(bat, -25);
			}

			bat->latency.number++;

		} else {
			/* Happens when an early noise comes in */
			bat->latency.state = LATENCY_STATE_WAITING;
			bat->latency.wait_samples -= (facc_sz + bat->period_size * 10);
			log_tdm2("Test%d %d+%d(%d) -> LATENCY_STATE_WAITING"
					", detected sound in first period"
					", wait_samples: %d\n",
					num, facc_pos, facc_sz, bat->latency.samples,
					bat->latency.wait_samples);
		}

	} else {
		/* Still listening */
		bat->latency.samples += frames;

		/* Do not listen to more than max device latency
		   Maybe too much background noise */
		if (bat->latency.samples > bat->latency.wait_samples) {
			bat->latency.error++;

			if (bat->latency.error > LATENCY_TEST_NUMBER) {
				fprintf(bat->err,
					_("Could not detect signal."));
				fprintf(bat->err,
					_("Too much background noise?\n"));
				bat->latency.state =
					LATENCY_STATE_COMPLETE_FAILURE;
				bat->latency.is_capturing = false;
				return;
			}

			/* let's start over */
			bat->latency.state = LATENCY_STATE_WAITING;
			bat->latency.wait_samples = bat->period_size * 10;
			log_tdm2("Test%d %d+%d -> LATENCY_STATE_WAITING"
					", miss sound"
					", err: %d, wait_samples: %d\n",
					num, facc_pos, facc_sz, bat->latency.error,
					bat->latency.wait_samples);
			dump_test_db_num(bat, bat->latency.test_db_num);
		}
	}
	if (bat->latency.state == LATENCY_STATE_WAITING) {
		bat->latency.samples = 0;
	}

	return;
}

static void calculate_threshold(struct bat *bat)
{
	float average;
	float reference;

	/* Calculate the average loudness of the environment and create
	   a threshold value 16 decibels higher than the average loudness */
	average = bat->latency.sum / bat->latency.samples / 32767.0f;
	bat->latency.measure_avgdb = 20.0f * log10f(average);
	reference = bat->latency.measure_avgdb + 16.0f;
	bat->latency.threshold = (int) (powf(10.0f, reference / 20.0f)
						* 32767.0f);
}

void roundtrip_latency_init(struct bat *bat)
{
#define roundup(_v, _d) ((((int)(_v) + (int)(_d) - 1) / (int)(_d)) * (int)(_d))

	bat->latency.number = 1;
	bat->latency.state = LATENCY_STATE_MEASURE_FOR_1_SECOND_SKIP_LEAD;
	bat->latency.latest_playback_state = bat->latency.state;
	bat->latency.final_result = 0;
	bat->latency.samples = 0;
	bat->latency.measure_skip_samples = bat->rate * 2 / 10;
	bat->latency.measure_skip_samples = roundup(
			bat->latency.measure_skip_samples, bat->period_size);
	bat->latency.sum = 0;
	bat->latency.threshold = 0;
	bat->latency.is_capturing = false;
	bat->latency.is_playing = false;
	bat->latency.error = 0;
	bat->latency.xrun_error = false;
	bat->frames = LATENCY_TEST_TIME_LIMIT * bat->rate;
	bat->periods_played = 0;
}

int handleinput(struct bat *bat, void *buffer, int frames)
{
	if (frames != bat->period_size) {
		log_tdm("Sanity check frames != bat->period_size");
		exit(1);
	}
	if (bat->channels != 1) {
		log_tdm("Sanity check bat->channels != 1");
		exit(1);
	}

	switch (bat->latency.state) {
	/* Measuring average loudness for 1 second */
	case LATENCY_STATE_MEASURE_FOR_1_SECOND_SKIP_LEAD:
		if (bat->latency.measure_skip_samples > 0) {
			bat->latency.samples += frames;
			if (bat->latency.samples >= bat->latency.measure_skip_samples) {
				log_tdm2("LATENCY_STATE_MEASURE_FOR_1_SECOND_SKIP_LEAD done for %d+%d\n",
						(bat->capture.facc - (bat->latency.samples - frames)),
						bat->latency.samples);
				bat->latency.samples = 0;
//				bat->latency.sum = 0;
				bat->latency.state = LATENCY_STATE_MEASURE_FOR_1_SECOND;
			}
			break;
		}
		// fallthrough to LATENCY_STATE_MEASURE_FOR_1_SECOND
		bat->latency.samples = 0;
//		bat->latency.sum = 0;
		bat->latency.state = LATENCY_STATE_MEASURE_FOR_1_SECOND;
	case LATENCY_STATE_MEASURE_FOR_1_SECOND:
		bat->latency.sum += sumaudio(bat, buffer, frames);
		bat->latency.samples += frames;

		/* 1 second elapsed */
		if (bat->latency.samples >=
				(bat->rate - bat->latency.measure_skip_samples)) {
			calculate_threshold(bat);

			log_tdm2("LATENCY_STATE_MEASURE_FOR_1_SECOND done for %d+%d"
					"; sum: %f, avgdb: %f, threshold: %d\n",
					(bat->capture.facc - (bat->latency.samples - frames)),
					bat->latency.samples,
					bat->latency.sum, bat->latency.measure_avgdb, bat->latency.threshold);

			bat->latency.state = LATENCY_STATE_PLAY_AND_LISTEN;
			bat->latency.samples = 0;
			bat->latency.sum = 0;
			bat->latency.wait_samples = bat->rate * LATENCY_TEST_MAX;
			bat->latency.silence_artifact = bat->latency.silence_artifact_def;
			bat->latency.test_db_num = 0;

		}
		break;

	/* Playing sine wave and listening if it comes back */
	case LATENCY_STATE_PLAY_AND_LISTEN:
		play_and_listen(bat, buffer, frames);
		break;

	/* Waiting 2 second (speaker may still playing for circuit latency) */
	case LATENCY_STATE_WAITING:
		bat->latency.samples += frames;

//		log_tdm("LATENCY_STATE_WAITING, samples: %d\n", (int)bat->latency.samples);

		if (bat->latency.samples > bat->rate * 3) {
			/* 3 second elapsed, start over */
			bat->latency.samples = 0;
			bat->latency.sum = 0;
			bat->latency.state = LATENCY_STATE_MEASURE_FOR_1_SECOND_SKIP_LEAD;
		}
		break;

	default:
		return 0;
	}

	return 0;
}

int handleoutput(struct bat *bat, void *buffer, int bytes, int frames)
{
	int err = 0;

	/* If capture completed, terminate the playback */
	if (bat->periods_played * frames > 2 * bat->rate
			&& bat->latency.is_capturing == false)
		return bat->latency.state;

	if (bat->latency.state == LATENCY_STATE_PLAY_AND_LISTEN) {
		if (bat->latency.latest_playback_state != bat->latency.state) {
			log_tdm2("playback state %d -> LATENCY_STATE_PLAY_AND_LISTEN(%d)\n",
					bat->latency.latest_playback_state,
					LATENCY_STATE_PLAY_AND_LISTEN);
		}
		if (bat->latency.silence_artifact > 0) {
			/* Output silence */
			memset(buffer, 0, bytes);
			bat->latency.silence_artifact--;
		} else {
		err = generate_sine_wave(bat, frames, buffer);
		}
	} else {
		if (bat->latency.latest_playback_state != bat->latency.state) {
			log_tdm2("playback state %d -> %d\n",
					bat->latency.latest_playback_state,
					bat->latency.state);
		}
		/* Output silence */
		memset(buffer, 0, bytes);
	}
	bat->latency.latest_playback_state = bat->latency.state;

	return err;
}
