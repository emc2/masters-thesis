/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#include <stdint.h>

#include "definitions.h"
#include "mm/lf_malloc_data.h"

static const uint64_t anchor_avail_mask =   0xffc0000000000000;
static const uint64_t anchor_credits_mask = 0x003ff00000000000;
static const uint64_t anchor_state_mask =   0x00000c0000000000;
static const uint64_t anchor_tag_mask =     0x000003ffffffffff;
static const uint64_t anchor_avail_shift = 54;
static const uint64_t anchor_credits_shift = 44;
static const uint64_t anchor_state_shift = 42;
static const unsigned int active_credits_mask = 0x0000003f;
static const unsigned int active_ptr_mask =  0xffffffc0;

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
internal pure anchor_t anchor_create(const unsigned int avail,
				     const unsigned int credits,
				     const unsigned int state) {

  const uint64_t avail64 = avail;
  const uint64_t credits64 = credits;
  const uint64_t state64 = state;
  const uint64_t avail_val =
    (avail64 << anchor_avail_shift) & anchor_avail_mask;
  const uint64_t credits_val =
    (credits64 << anchor_credits_shift) & anchor_credits_mask;
  const uint64_t state_val =
    (state64 << anchor_state_shift) & anchor_state_mask;
  const uint64_t tag_val = 0;

  return avail_val | credits_val | state_val | tag_val;

}


/*!
 * This function extracts the avail field from an anchor.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the avail field from an anchor.
 * \arg anchor The anchor structure.
 * \return The avail field (a 10-bit unsigned integer).
 */
internal pure unsigned int anchor_get_avail(const anchor_t anchor) {

  return (anchor & anchor_avail_mask) >> anchor_avail_shift;

}


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
internal pure anchor_t anchor_set_avail(const anchor_t anchor,
					const unsigned int avail) {

  const uint64_t avail64 = avail;
  const uint64_t avail_val =
    (avail64 << anchor_avail_shift) & anchor_avail_mask;
  const uint64_t credits_val = anchor & anchor_credits_mask;
  const uint64_t state_val = anchor & anchor_state_mask;
  const uint64_t tag_val = anchor & anchor_tag_mask;

  return avail_val | credits_val | state_val | tag_val;

}


/*!
 * This function extracts the count field from an anchor.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the count field from an anchor.
 * \arg anchor The anchor structure.
 * \return The count field (a 10-bit unsigned integer).
 */
internal pure unsigned int anchor_get_credits(const anchor_t anchor) {

  return (anchor & anchor_credits_mask) >> anchor_credits_shift;

}


/*!
 * This function sets the count field in an anchor structure.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Set the credits field in an anchor.
 * \arg anchor The anchor structure.
 * \arg count The credits field (10-bit unsigned integer).
 * \return The new anchor structure.
 */
internal pure anchor_t anchor_set_credits(const anchor_t anchor,
					  const unsigned int credits) {

  const uint64_t credits64 = credits;
  const uint64_t avail_val = anchor & anchor_avail_mask;
  const uint64_t credits_val =
    (credits64 << anchor_credits_shift) & anchor_credits_mask;
  const uint64_t state_val = anchor & anchor_state_mask;
  const uint64_t tag_val = anchor & anchor_tag_mask;

  return avail_val | credits_val | state_val | tag_val;

}


/*!
 * This function extracts the state field from an anchor.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the state field from an anchor.
 * \arg anchor The anchor structure.
 * \return The state field (a 2-bit unsigned integer).
 */
internal pure unsigned int anchor_get_state(const anchor_t anchor) {

  return (anchor & anchor_state_mask) >> anchor_state_shift;

}


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
internal pure anchor_t anchor_set_state(const anchor_t anchor,
					const unsigned int state) {

  const uint64_t state64 = state;
  const uint64_t avail_val = anchor & anchor_avail_mask;
  const uint64_t credits_val = anchor & anchor_credits_mask;
  const uint64_t state_val =
    (state64 << anchor_state_shift) & anchor_state_mask;
  const uint64_t tag_val = anchor & anchor_tag_mask;

  return avail_val | credits_val | state_val | tag_val;

}


/*!
 * This function extracts the tag field from an anchor.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the tag field from an anchor.
 * \arg anchor The anchor structure.
 * \return The tag field (a 48-bit unsigned integer).
 */
internal pure uint64_t anchor_get_tag(const anchor_t anchor) {

  return anchor & anchor_tag_mask;

}


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
internal pure anchor_t anchor_set_tag(const anchor_t anchor,
				      const uint64_t tag) {

  const uint64_t avail_val = anchor & anchor_avail_mask;
  const uint64_t credits_val = anchor & anchor_credits_mask;
  const uint64_t state_val = anchor & anchor_state_mask;
  const uint64_t tag_val = tag & anchor_tag_mask;

  return avail_val | credits_val | state_val | tag_val;

}


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
internal pure active_t active_create(const void* const restrict ptr,
				     const unsigned int credits) {

  const unsigned int ptr_val = (unsigned int)ptr & active_ptr_mask;
  const unsigned int credits_val = credits & active_credits_mask;

  return ptr_val | credits_val;

}


/*!
 * This function extracts the credits field from an active.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the credits field from an active.
 * \arg active The active structure.
 * \return The credits field (a 6-bit unsigned integer).
 */
internal pure unsigned int active_get_credits(const active_t active) {

  return active & active_credits_mask;

}


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
internal pure active_t active_set_credits(const active_t active,
					  const unsigned int credits) {

  const unsigned int ptr_val = active & active_ptr_mask;
  const unsigned int credits_val = credits & active_credits_mask;

  return ptr_val | credits_val;

}


/*!
 * This function extracts the ptr field from an active.  This is
 * actually a simple bitwise operation in most cases.  This exists
 * primarily to keep the types clean in main code.
 *
 * \brief Get the ptr field from an active.
 * \arg active The active structure.
 * \return The ptr field.
 */
internal pure void* active_get_ptr(const active_t active) {

  return (void*)(active & active_ptr_mask);

}


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
internal pure active_t active_set_ptr(const active_t active,
				      const void* const ptr) {

  const unsigned int ptr_val = (unsigned int)ptr & active_ptr_mask;
  const unsigned int credits_val = active & active_credits_mask;

  return ptr_val | credits_val;

}
