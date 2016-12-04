/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#ifndef LF_MALLOC_DATA_H
#define LF_MALLOC_DATA_H

#include <stdint.h>

#include "definitions.h"
#include "arch.h"

typedef uint64_t anchor_t;

#if (32 == BITS)
typedef uint32_t active_t;
#elif (64 == BITS)
typedef uint64_t active_t;
#else
#error "Invalid word size"
#endif

#define MAX_CREDITS 64

/*!
 * This function creates an anchor value from avail, state, and count
 * values.  This is a simple bitwise operation in most cases.  This
 * exists primarily to keep the types clean in main code.
 *
 * \brief Create an active value.
 * \arg avail The avail value.
 * \arg count The count value.
 * \arg state The state value.
 * \return The anchor value.
 */
internal pure anchor_t anchor_create(unsigned int avail, unsigned int credits,
				     unsigned int state);

/*!
 * This function extracts the avail field from an anchor.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the avail field from an anchor.
 * \arg anchor The anchor structure.
 * \return The avail field (a 10-bit unsigned integer).
 */
internal pure unsigned int anchor_get_avail(anchor_t anchor);


/*!
 * This function sets the avail field in an anchor structure.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Set the avail field in an anchor.
 * \arg anchor The anchor structure.
 * \arg avail The avail field (10-bit unsigned integer).
 * \return The new anchor structure.
 */
internal pure anchor_t anchor_set_avail(anchor_t anchor, unsigned int avail);


/*!
 * This function extracts the credits field from an anchor.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the credits field from an anchor.
 * \arg anchor The anchor structure.
 * \return The count field (a 10-bit unsigned integer).
 */
internal pure unsigned int anchor_get_credits(anchor_t anchor);


/*!
 * This function sets the credits field in an anchor structure.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Set the credits field in an anchor.
 * \arg anchor The anchor structure.
 * \arg count The credits field (10-bit unsigned integer).
 * \return The new anchor structure.
 */
internal pure anchor_t anchor_set_credits(anchor_t anchor,
					  unsigned int credits);


/*!
 * This function extracts the state field from an anchor.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the state field from an anchor.
 * \arg anchor The anchor structure.
 * \return The state field (a 2-bit unsigned integer).
 */
internal pure unsigned int anchor_get_state(anchor_t anchor);


/*!
 * This function sets the state field in an anchor structure.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Set the state field in an anchor.
 * \arg anchor The anchor structure.
 * \arg state The state field (2-bit unsigned integer).
 * \return The new anchor structure.
 */
internal pure anchor_t anchor_set_state(anchor_t anchor, unsigned int state);


/*!
 * This function extracts the tag field from an anchor.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the tag field from an anchor.
 * \arg anchor The anchor structure.
 * \return The tag field (a 48-bit unsigned integer).
 */
internal pure uint64_t anchor_get_tag(anchor_t anchor);


/*!
 * This function sets the tag field in an anchor structure.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Set the tag field in an anchor.
 * \arg anchor The anchor structure.
 * \arg tag The tag field (48-bit unsigned integer).
 * \return The new anchor structure.
 */
internal pure anchor_t anchor_set_tag(anchor_t anchor, uint64_t tag);

/*!
 * This function creates an active value from a pointer and credits
 * value.  This is a simple bitwise operation in most cases.  This
 * exists primarily to keep the types clean in main code.
 *
 * \brief Create an active value.
 * \arg ptr The pointer value.
 * \arg credits The credits value.
 * \return The active value.
 */
internal pure active_t active_create(const void* restrict ptr,
				     unsigned int credits);

/*!
 * This function extracts the credits field from an active.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the credits field from an active.
 * \arg active The active structure.
 * \return The credits field (a 6-bit unsigned integer).
 */
internal pure unsigned int active_get_credits(active_t active);


/*!
 * This function sets the credits field in an active structure.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Set the credits field in an active.
 * \arg active The active structure.
 * \arg credits The credits field (6-bit unsigned integer).
 * \return The new active structure.
 */
internal pure active_t active_set_credits(active_t active,
					  unsigned int credits);


/*!
 * This function extracts the ptr field from an active.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the ptr field from an active.
 * \arg active The active structure.
 * \return The ptr field.
 */
internal pure void* active_get_ptr(active_t active);


/*!
 * This function sets the ptr field in an active structure.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Set the ptr field in an active.
 * \arg active The active structure.
 * \arg ptr The ptr field.
 * \return The new active structure.
 */
internal pure active_t active_set_ptr(active_t active, const void* ptr);

#endif
