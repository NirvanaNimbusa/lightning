#include <ccan/array_size/array_size.h>
#include <ccan/cast/cast.h>
#include <common/fee_states.h>
#include <common/type_to_string.h>
#include <wire/wire.h>

/* If we're the finder, it's like an HTLC we added, if they are, it's like
 * a HTLC they added. */
enum htlc_state first_fee_state(enum side funder)
{
	if (funder == LOCAL)
		return SENT_ADD_HTLC;
	else
		return RCVD_ADD_HTLC;
}

enum htlc_state last_fee_state(enum side funder)
{
	if (funder == LOCAL)
		return SENT_ADD_ACK_REVOCATION;
	else
		return RCVD_ADD_ACK_REVOCATION;
}

struct fee_states *new_fee_states(const tal_t *ctx,
				  enum side funder,
				  const u32 *feerate_per_kw)
{
	struct fee_states *fee_states = tal(ctx, struct fee_states);

	/* Set to NULL except terminal value */
	for (size_t i = 0; i < ARRAY_SIZE(fee_states->feerate); i++)
		fee_states->feerate[i] = NULL;
	if (feerate_per_kw)
		fee_states->feerate[last_fee_state(funder)]
			= tal_dup(fee_states, u32, feerate_per_kw);
	return fee_states;
}

struct fee_states *dup_fee_states(const tal_t *ctx,
				  const struct fee_states *fee_states TAKES)
{
	struct fee_states *n;

	if (taken(fee_states))
		return cast_const(struct fee_states *,
				  tal_steal(ctx, fee_states));
	n = tal_dup(ctx, struct fee_states, fee_states);
	for (size_t i = 0; i < ARRAY_SIZE(n->feerate); i++) {
		if (n->feerate[i])
			n->feerate[i] = tal_dup(n, u32, n->feerate[i]);
	}
	return n;
}

u32 get_feerate(const struct fee_states *fee_states,
		enum side funder,
		enum side side)
{
	/* The first non-NULL feerate committed to this side is current */
	for (enum htlc_state i = first_fee_state(funder);
	     i <= last_fee_state(funder);
	     i++) {
		if (!fee_states->feerate[i])
			continue;
		if (!(htlc_state_flags(i) & HTLC_FLAG(side, HTLC_F_COMMITTED)))
			continue;
		return *fee_states->feerate[i];
	}

	/* Some feerate should always be set! */
	abort();
}

void start_fee_update(struct fee_states *fee_states,
		      enum side funder,
		      u32 feerate_per_kw)
{
	enum htlc_state start = first_fee_state(funder);

	/* BOLT #2:
	 * Unlike an HTLC, `update_fee` is never closed but simply replaced.
	 */
	if (fee_states->feerate[start] == NULL)
		fee_states->feerate[start] = tal(fee_states, u32);
	*fee_states->feerate[start] = feerate_per_kw;
}

bool inc_fee_state(struct fee_states *fee_states, enum htlc_state hstate)
{
	/* These only advance through ADDING states. */
	assert(htlc_state_flags(hstate) & HTLC_ADDING);

	if (!fee_states->feerate[hstate])
		return false;

	/* FIXME: We can never clash, except at final state unless someone
	 * has violated protocol (eg, send two revoke_and_ack back-to-back) */
	tal_free(fee_states->feerate[hstate+1]);
	fee_states->feerate[hstate+1] = fee_states->feerate[hstate];
	fee_states->feerate[hstate] = NULL;
	return true;
}

struct fee_states *fromwire_fee_states(const tal_t *ctx,
				       const u8 **cursor, size_t *max)
{
	struct fee_states *fee_states = tal(ctx, struct fee_states);

	for (enum htlc_state i = 0; i < ARRAY_SIZE(fee_states->feerate); i++) {
		if (fromwire_bool(cursor, max)) {
			fee_states->feerate[i] = tal(fee_states, u32);
			*fee_states->feerate[i] = fromwire_u32(cursor, max);
		} else {
			fee_states->feerate[i] = NULL;
		}
	}
	if (!*cursor)
		return tal_free(fee_states);
	return fee_states;
}

void towire_fee_states(u8 **pptr, const struct fee_states *fee_states)
{
	for (enum htlc_state i = 0; i < ARRAY_SIZE(fee_states->feerate); i++) {
		/* We don't send uncommitted feestates */
		if (!(htlc_state_flags(i) & (HTLC_REMOTE_F_COMMITTED
					     | HTLC_LOCAL_F_COMMITTED))
		    || fee_states->feerate[i] == NULL) {
			towire_bool(pptr, false);
			continue;
		}
		towire_bool(pptr, true);
		towire_u32(pptr, *fee_states->feerate[i]);
	}
}

/* FIXME: we don't know funder inside fromwire_fee_states, so can't do
 * this there :( */
bool fee_states_valid(const struct fee_states *fee_states, enum side funder)
{
	return fee_states->feerate[last_fee_state(funder)] != NULL;
}

static const char *fmt_fee_states(const tal_t *ctx,
				  const struct fee_states *fee_states)
{
	char *ret = tal_strdup(ctx, "{");
	for (enum htlc_state i = 0; i < ARRAY_SIZE(fee_states->feerate); i++) {
		if (fee_states->feerate[i] != NULL)
			tal_append_fmt(&ret, " %s:%u",
				       htlc_state_name(i),
				       *fee_states->feerate[i]);
	}
	tal_append_fmt(&ret, " }");
	return ret;
}
REGISTER_TYPE_TO_STRING(fee_states, fmt_fee_states);
