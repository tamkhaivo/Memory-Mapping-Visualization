
TEST_F(VisualizationArenaTest, TwoArenasOneThread) {
  auto result_b = VisualizationArena::create({.arena_size = 1024 * 1024});
  ASSERT_TRUE(result_b.has_value());
  // Move to a unique_ptr to manage lifetime explicitly if needed, but Test
  // fixture manages arena_ Here we use a local arena_b
  VisualizationArena arena_b = std::move(*result_b);

  // 1. Alloc in A (registers context in A)
  arena_->alloc_raw(16, 16, "A1");

  // 2. Alloc in B (deletes A's context, registers B's context)
  arena_b.alloc_raw(16, 16, "B1");

  // 3. Alloc in A again (deletes B's context, registers NEW context in A)
  // At this point, A has TWO pointers in active_contexts. One comes from step 1
  // (freed), one from step 3 (valid).
  arena_->alloc_raw(16, 16, "A2");

  // 4. Trigger iteration over active_contexts in A
  // This should crash if A tries to access the freed context from step 1.
  auto json = arena_->event_log_json();
  EXPECT_FALSE(json.empty());
}
