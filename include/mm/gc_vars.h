/* Copyright 2008 Eric McCorkle.  All rights reserved. */

#ifndef GC_VARS_H
#define GC_VARS_H

#define GC_MAX_GENS 254

#define GC_CLUSTER_SIZE 16

/*!
 * This is the default number of generations used by the garbage
 * collector.  This should not exceed about 4, and cannot exceed 254.
 *
 * \brief The number of generations.
 */
internal unsigned int gc_num_generations;


/*!
 * This is the lowest generation a large array will occupy.  This
 * should be at least 1, to prevent large arrays from being copied
 * many times over.  However, it should not be the highest generation,
 * or else dead arrays will cause many objects to be retained that
 * otherwise would not be.
 *
 * \brief The generation for large arrays.
 */
internal unsigned int gc_array_gen;


#endif
