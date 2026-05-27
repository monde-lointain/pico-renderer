#include "cmd/cmd.h"

#include <stdint.h>
#include <string.h>

#include "gtest/gtest.h"

// ---- helpers ---------------------------------------------------------------

static struct Command mk_clear(uint16_t color) {
  struct Command c;
  memset(&c, 0, sizeof c);
  c.op = CMD_CLEAR;
  c.u.clear.color = color;
  return c;
}

static struct Command mk_op(uint8_t op) {
  struct Command c;
  memset(&c, 0, sizeof c);
  c.op = op;
  return c;
}

static struct Command mk_call(const struct Command* dl) {
  struct Command c;
  memset(&c, 0, sizeof c);
  c.op = CMD_CALL_LIST;
  c.u.call_list.ptr = dl;
  return c;
}

static struct Command mk_branch(const struct Command* dl) {
  struct Command c;
  memset(&c, 0, sizeof c);
  c.op = CMD_BRANCH_LESS_Z;
  c.u.branch_less_z.dl = dl;
  return c;
}

// Visitor that records the op of every command it sees, plus a programmable
// branch-predicate answer.
struct VisitLog {
  uint8_t ops[64];
  int n;
  int take_branch;  // value returned for CMD_BRANCH_LESS_Z
};

static int log_visit(void* ctx, const struct Command* c) {
  struct VisitLog* v = (struct VisitLog*)ctx;
  if (v->n < (int)(sizeof v->ops)) {
    v->ops[v->n] = c->op;
    v->n++;
  }
  return v->take_branch;
}

// ============================================================================
// record: cb_reset / cb_push / overflow drop-with-count
// ============================================================================

TEST(CmdRecord, ResetZeroesCountAndDropped) {
  struct Command storage[4];
  struct CmdBuf cb;
  cb.buf = storage;
  cb.cap = 4;
  cb.count = 99;
  cb.dropped = 7;
  cb_reset(&cb);
  EXPECT_EQ(cb.count, 0U);
  EXPECT_EQ(cb.dropped, 0U);
}

TEST(CmdRecord, PushAppendsAndCopiesPayload) {
  struct Command storage[4];
  struct CmdBuf cb;
  cb.buf = storage;
  cb.cap = 4;
  cb_reset(&cb);
  struct Command const c = mk_clear(0xBEEF);
  EXPECT_EQ(cb_push(&cb, &c), RDR_OK);
  EXPECT_EQ(cb.count, 1U);
  EXPECT_EQ(cb.buf[0].op, (uint8_t)CMD_CLEAR);
  EXPECT_EQ(cb.buf[0].u.clear.color, 0xBEEFU);
}

TEST(CmdRecord, PushIsByValueCopyNotAlias) {
  struct Command storage[2];
  struct CmdBuf cb;
  cb.buf = storage;
  cb.cap = 2;
  cb_reset(&cb);
  struct Command c = mk_clear(0x1111);
  cb_push(&cb, &c);
  c.u.clear.color = 0x2222;                     // mutate source after push
  EXPECT_EQ(cb.buf[0].u.clear.color, 0x1111U);  // buffer holds the snapshot
}

TEST(CmdRecord, PushFillsToExactCapacity) {
  struct Command storage[3];
  struct CmdBuf cb;
  cb.buf = storage;
  cb.cap = 3;
  cb_reset(&cb);
  struct Command const c = mk_op(CMD_END);
  EXPECT_EQ(cb_push(&cb, &c), RDR_OK);
  EXPECT_EQ(cb_push(&cb, &c), RDR_OK);
  EXPECT_EQ(cb_push(&cb, &c), RDR_OK);
  EXPECT_EQ(cb.count, 3U);
  EXPECT_EQ(cb.dropped, 0U);
}

TEST(CmdRecord, OverflowDropsWithCountNeverCorrupts) {
  struct Command storage[2];
  struct CmdBuf cb;
  cb.buf = storage;
  cb.cap = 2;
  cb_reset(&cb);
  struct Command const keep0 = mk_clear(0xAAAA);
  struct Command const keep1 = mk_clear(0xBBBB);
  struct Command const drop = mk_clear(0xCCCC);
  EXPECT_EQ(cb_push(&cb, &keep0), RDR_OK);
  EXPECT_EQ(cb_push(&cb, &keep1), RDR_OK);
  EXPECT_EQ(cb_push(&cb, &drop), RDR_EOVERFLOW);  // dropped, surfaced
  EXPECT_EQ(cb_push(&cb, &drop), RDR_EOVERFLOW);  // and counted
  EXPECT_EQ(cb.count, 2U);                        // count never exceeds cap
  EXPECT_EQ(cb.dropped, 2U);                      // both drops counted
  EXPECT_EQ(cb.buf[0].u.clear.color, 0xAAAAU);    // earlier entries intact
  EXPECT_EQ(cb.buf[1].u.clear.color, 0xBBBBU);
}

// ============================================================================
// replay: cb_walk linear stream, terminated by END
// ============================================================================

TEST(CmdWalk, NullArgsRejected) {
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  struct Command const end = mk_op(CMD_END);
  EXPECT_EQ(cb_walk(0, log_visit, &v), RDR_EINVAL);
  EXPECT_EQ(cb_walk(&end, 0, &v), RDR_EINVAL);
}

TEST(CmdWalk, LinearStreamVisitsEachNonControlCmdInOrder) {
  struct Command dl[4];
  dl[0] = mk_op(CMD_SET_VIEWPORT);
  dl[1] = mk_clear(0x07E0);
  dl[2] = mk_op(CMD_DRAW_TRIS);
  dl[3] = mk_op(CMD_END);
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  EXPECT_EQ(cb_walk(dl, log_visit, &v), RDR_OK);
  ASSERT_EQ(v.n, 3);  // END is control flow, not visited
  EXPECT_EQ(v.ops[0], (uint8_t)CMD_SET_VIEWPORT);
  EXPECT_EQ(v.ops[1], (uint8_t)CMD_CLEAR);
  EXPECT_EQ(v.ops[2], (uint8_t)CMD_DRAW_TRIS);
}

TEST(CmdWalk, EndTerminatesBeforeTrailingCommands) {
  struct Command dl[3];
  dl[0] = mk_clear(1);
  dl[1] = mk_op(CMD_END);
  dl[2] = mk_clear(2);  // past END — must never be visited
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  EXPECT_EQ(cb_walk(dl, log_visit, &v), RDR_OK);
  ASSERT_EQ(v.n, 1);
  EXPECT_EQ(v.ops[0], (uint8_t)CMD_CLEAR);
}

// ============================================================================
// CALL_LIST + RETURN: push/replay/return; END pops one frame
// ============================================================================

TEST(CmdWalk, CallListReplaysSubListThenReturns) {
  static struct Command sub[3];
  sub[0] = mk_op(CMD_SET_MATRIX);
  sub[1] = mk_op(CMD_DRAW_TRIS);
  sub[2] = mk_op(CMD_RETURN);
  struct Command main_dl[4];
  main_dl[0] = mk_clear(0);
  main_dl[1] = mk_call(sub);
  main_dl[2] = mk_op(CMD_SET_FOG);  // resume here after RETURN
  main_dl[3] = mk_op(CMD_END);
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  EXPECT_EQ(cb_walk(main_dl, log_visit, &v), RDR_OK);
  ASSERT_EQ(v.n, 4);
  EXPECT_EQ(v.ops[0], (uint8_t)CMD_CLEAR);
  EXPECT_EQ(v.ops[1], (uint8_t)CMD_SET_MATRIX);
  EXPECT_EQ(v.ops[2], (uint8_t)CMD_DRAW_TRIS);
  EXPECT_EQ(v.ops[3], (uint8_t)CMD_SET_FOG);
}

TEST(CmdWalk, EndInSubListActsLikeReturn) {
  static struct Command sub[2];
  sub[0] = mk_op(CMD_DRAW_TRIS);
  sub[1] = mk_op(CMD_END);  // END inside a called list returns to caller
  struct Command main_dl[3];
  main_dl[0] = mk_call(sub);
  main_dl[1] = mk_clear(5);  // must resume here
  main_dl[2] = mk_op(CMD_END);
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  EXPECT_EQ(cb_walk(main_dl, log_visit, &v), RDR_OK);
  ASSERT_EQ(v.n, 2);
  EXPECT_EQ(v.ops[0], (uint8_t)CMD_DRAW_TRIS);
  EXPECT_EQ(v.ops[1], (uint8_t)CMD_CLEAR);
}

TEST(CmdWalk, NestedCallLists) {
  static struct Command inner[2];
  inner[0] = mk_op(CMD_DRAW_TRIS);
  inner[1] = mk_op(CMD_RETURN);
  static struct Command outer[3];
  outer[0] = mk_op(CMD_SET_MATRIX);
  outer[1] = mk_call(inner);
  outer[2] = mk_op(CMD_RETURN);
  struct Command main_dl[3];
  main_dl[0] = mk_call(outer);
  main_dl[1] = mk_clear(9);
  main_dl[2] = mk_op(CMD_END);
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  EXPECT_EQ(cb_walk(main_dl, log_visit, &v), RDR_OK);
  ASSERT_EQ(v.n, 3);
  EXPECT_EQ(v.ops[0], (uint8_t)CMD_SET_MATRIX);
  EXPECT_EQ(v.ops[1], (uint8_t)CMD_DRAW_TRIS);
  EXPECT_EQ(v.ops[2], (uint8_t)CMD_CLEAR);
}

TEST(CmdWalk, NullCallTargetRejected) {
  struct Command main_dl[2];
  main_dl[0] = mk_call(0);
  main_dl[1] = mk_op(CMD_END);
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  EXPECT_EQ(cb_walk(main_dl, log_visit, &v), RDR_EINVAL);
}

TEST(CmdWalk, CallNestingOverflowRejected) {
  // A list that calls itself forever overflows the bounded return stack.
  static struct Command rec[2];
  rec[0] = mk_call(rec);  // self-call: never returns
  rec[1] = mk_op(CMD_END);
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  EXPECT_EQ(cb_walk(rec, log_visit, &v), RDR_EOVERFLOW);
}

// ============================================================================
// BRANCH_LESS_Z: no-push tail-jump when predicate taken; fall through else
// ============================================================================

TEST(CmdWalk, BranchTakenTailJumpsNoReturn) {
  static struct Command lod[2];
  lod[0] = mk_op(CMD_DRAW_TRIS);  // the low-LOD list
  lod[1] = mk_op(CMD_END);
  struct Command main_dl[3];
  main_dl[0] = mk_branch(lod);
  main_dl[1] = mk_clear(0xDEAD);  // skipped when branch taken (no-push jump)
  main_dl[2] = mk_op(CMD_END);
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  v.take_branch = 1;
  EXPECT_EQ(cb_walk(main_dl, log_visit, &v), RDR_OK);
  ASSERT_EQ(v.n, 2);
  EXPECT_EQ(v.ops[0], (uint8_t)CMD_BRANCH_LESS_Z);  // predicate consult visited
  EXPECT_EQ(v.ops[1], (uint8_t)CMD_DRAW_TRIS);      // jumped; did not return
}

TEST(CmdWalk, BranchNotTakenFallsThrough) {
  static struct Command lod[2];
  lod[0] = mk_op(CMD_DRAW_TRIS);
  lod[1] = mk_op(CMD_END);
  struct Command main_dl[3];
  main_dl[0] = mk_branch(lod);
  main_dl[1] = mk_clear(0x1234);  // executed when branch not taken
  main_dl[2] = mk_op(CMD_END);
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  v.take_branch = 0;
  EXPECT_EQ(cb_walk(main_dl, log_visit, &v), RDR_OK);
  ASSERT_EQ(v.n, 2);
  EXPECT_EQ(v.ops[0], (uint8_t)CMD_BRANCH_LESS_Z);
  EXPECT_EQ(v.ops[1], (uint8_t)CMD_CLEAR);
}

TEST(CmdWalk, BranchNullTargetRejected) {
  struct Command main_dl[2];
  main_dl[0] = mk_branch(0);
  main_dl[1] = mk_op(CMD_END);
  struct VisitLog v;
  memset(&v, 0, sizeof v);
  v.take_branch = 1;
  EXPECT_EQ(cb_walk(main_dl, log_visit, &v), RDR_EINVAL);
}
