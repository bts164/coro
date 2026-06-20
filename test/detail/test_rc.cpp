// Compile-time exercise for coro::detail::Rc<T>/Weak<T>.
//
// Only meaningful under CORO_PICO, where Rc/Weak resolve to the custom
// non-atomic NonAtomicRc/NonAtomicWeak (see include/coro/detail/rc.h).
// Off CORO_PICO they are aliases for std::shared_ptr/std::weak_ptr, so this
// file just exercises the standard library types on the desktop test suite.
// Linked into test_pico_suite (CORO_PICO defined) by test/CMakeLists.txt.

#include <coro/detail/rc.h>
#include <gtest/gtest.h>
#include <unordered_set>

using namespace coro::detail;

namespace {

struct Counted {
    explicit Counted(int v, int* live) : value(v), live(live) { ++*live; }
    ~Counted() { --*live; }
    int  value;
    int* live;
};

struct Base { virtual ~Base() = default; int tag = 1; };
struct Other { int pad = 0; };
// Multiple inheritance, mirroring TaskBase's Waker + (enable_shared_from_this) layout.
struct Derived : Other, Base { int tag2 = 2; };

// Mirrors TaskBase's set_self()/m_self pattern: a self-referential Weak<T>
// set right after construction, exactly like make_shared + enable_shared_from_this
// does implicitly on desktop.
struct SelfRef {
    explicit SelfRef(int* live) : live(live) { ++*live; }
    ~SelfRef() { --*live; }
    void set_self(Weak<SelfRef> self) { m_self = std::move(self); }
    int* live;
    Weak<SelfRef> m_self;
};

} // namespace

TEST(Rc, MakeRcConstructsValue) {
    int live = 0;
    Rc<Counted> p = make_rc<Counted>(42, &live);
    EXPECT_EQ(live, 1);
    EXPECT_EQ(p->value, 42);
    p.reset();
    EXPECT_EQ(live, 0);
}

TEST(Rc, CopyKeepsValueAliveUntilLastReleased) {
    int live = 0;
    Rc<Counted> a = make_rc<Counted>(1, &live);
    {
        Rc<Counted> b = a;
        EXPECT_EQ(live, 1);
    }
    EXPECT_EQ(live, 1);
    a.reset();
    EXPECT_EQ(live, 0);
}

TEST(Rc, MoveTransfersOwnership) {
    int live = 0;
    Rc<Counted> a = make_rc<Counted>(7, &live);
    Rc<Counted> b = std::move(a);
    EXPECT_EQ(a.get(), nullptr);
    EXPECT_EQ(b->value, 7);
    EXPECT_EQ(live, 1);
    b.reset();
    EXPECT_EQ(live, 0);
}

TEST(Rc, MoveAssignmentTransfersOwnershipAndReleasesOld) {
    int live_a = 0, live_b = 0;
    Rc<Counted> a = make_rc<Counted>(1, &live_a);
    Rc<Counted> b = make_rc<Counted>(2, &live_b);
    b = std::move(a);
    EXPECT_EQ(a.get(), nullptr);
    EXPECT_EQ(b->value, 1);
    EXPECT_EQ(live_a, 1); // value 1 still alive, now owned by b
    EXPECT_EQ(live_b, 0); // value 2 released when b was overwritten
    b.reset();
    EXPECT_EQ(live_a, 0);
}

TEST(Rc, UpcastAcrossMultipleInheritance) {
    Rc<Derived> d = make_rc<Derived>();
    Rc<Base> b = d; // implicit upcast
    EXPECT_EQ(b->tag, 1);
    EXPECT_EQ(b.get(), static_cast<Base*>(d.get()));
}

TEST(Rc, AliasingConstructorSharesOwnership) {
    int live = 0;
    Rc<Counted> owner = make_rc<Counted>(9, &live);
    Rc<int> alias(owner, &owner->value);
    owner.reset();
    EXPECT_EQ(live, 1); // alias still keeps the value alive
    EXPECT_EQ(*alias, 9);
    alias.reset();
    EXPECT_EQ(live, 0);
}

TEST(Weak, LockSucceedsWhileRcAlive) {
    int live = 0;
    Rc<Counted> a = make_rc<Counted>(3, &live);
    Weak<Counted> w = a;
    auto locked = w.lock();
    ASSERT_TRUE(locked);
    EXPECT_EQ(locked->value, 3);
}

TEST(Weak, LockFailsAfterRcDestroyed) {
    int live = 0;
    Weak<Counted> w;
    {
        Rc<Counted> a = make_rc<Counted>(5, &live);
        w = a;
        EXPECT_FALSE(w.expired());
    }
    EXPECT_TRUE(w.expired());
    EXPECT_FALSE(w.lock());
}

TEST(Weak, ConstructibleFromDerivedRc) {
    Rc<Derived> d = make_rc<Derived>();
    Weak<Base> w(d); // Weak<Base> from Rc<Derived> — used for TaskBase::m_self
    auto locked = w.lock();
    ASSERT_TRUE(locked);
    EXPECT_EQ(locked.get(), static_cast<Base*>(d.get()));
}

// Regression test: dropping the last strong ref to a value holding a
// self-referential Weak<T> (TaskBase::m_self) used to free the control block
// from inside that same value's own destructor (the self-weak's destructor
// is the last weak ref, sees strong_count == 0, and deleted m_block while
// destroy_value() — i.e. ~SelfRef() — was still running on it), causing a
// guaranteed use-after-free. release_strong()/release_weak() now defer the
// free decision until destroy_value() has fully returned.
TEST(Rc, SelfReferentialWeakDoesNotUseAfterFreeOnDestruction) {
    int live = 0;
    Rc<SelfRef> a = make_rc<SelfRef>(&live);
    a->set_self(Weak<SelfRef>(a));
    EXPECT_EQ(live, 1);
    a.reset(); // strong_count -> 0; destroying the value also destroys m_self
    EXPECT_EQ(live, 0);
}

// Same scenario, but with an external weak ref outstanding alongside the
// self-ref — verifies the deferred free logic doesn't disturb normal weak
// refcounting: the block must stay alive (for the external Weak to observe
// strong_count == 0) until that external ref is also released.
TEST(Rc, SelfReferentialWeakBlockSurvivesUntilExternalWeakAlsoReleased) {
    int live = 0;
    Rc<SelfRef> a = make_rc<SelfRef>(&live);
    a->set_self(Weak<SelfRef>(a));
    Weak<SelfRef> external = a;
    a.reset();
    EXPECT_EQ(live, 0);
    EXPECT_TRUE(external.expired());
    EXPECT_FALSE(external.lock());
}

TEST(Rc, UsableAsUnorderedSetKey) {
    std::unordered_set<Rc<Counted>> set;
    int live = 0;
    Rc<Counted> a = make_rc<Counted>(1, &live);
    Rc<Counted> b = a;
    set.insert(a);
    EXPECT_TRUE(set.contains(b));
}
