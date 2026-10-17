#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace coro::detail {

/**
 * @brief Base node for an intrusive doubly-linked list.
 *
 * A node must not be destroyed while it is still linked. Callers are
 * responsible for removing nodes before destruction (typically in a future's
 * destructor under the channel mutex) — `IntrusiveList::remove()` is always
 * safe to call on a node regardless of whether it is currently linked, so
 * callers should call it unconditionally rather than trying to track
 * linkage themselves.
 */
struct IntrusiveListNode {
    IntrusiveListNode* prev = nullptr;
    IntrusiveListNode* next = nullptr;
};

/**
 * @brief Intrusive doubly-linked list of nodes that carry their own links.
 *
 * Does not own the nodes — all storage is in the embedded nodes themselves.
 * Not thread-safe; callers must hold an external lock.
 *
 * The links are the members `Prev` and `Next` of the node, by default ones named
 * `prev` and `next`. Name others to put a type in a list through hooks it already
 * has under other names, or to link one node into two lists at once through two
 * pairs of hooks:
 *
 * ```cpp
 * IntrusiveList<TaskBase*, &TaskBase::owned_prev, &TaskBase::owned_next> owned;
 * ```
 *
 * Both hooks must be of type `NodePtr`, and the node type must be complete where
 * the list type is named.
 *
 * ### Complexity
 * | Operation            | Complexity |
 * |----------------------|------------|
 * | push_back            | O(1)       |
 * | pop_front / pop_back | O(1)       |
 * | remove               | O(1)       |
 * | push_back_block      | O(1)       |
 * | pop_back_block(n)    | O(n)       |
 *
 * @tparam NodePtr Pointer type stored in the list; defaults to `IntrusiveListNode*`.
 * @tparam Prev    The node's member linking to the previous node.
 * @tparam Next    The node's member linking to the next node.
 */
template<typename NodePtr = IntrusiveListNode*,
         auto Prev = &std::remove_pointer_t<NodePtr>::prev,
         auto Next = &std::remove_pointer_t<NodePtr>::next>
class IntrusiveList {
    static_assert(std::is_same_v<decltype(std::declval<NodePtr>()->*Prev), NodePtr&> &&
                  std::is_same_v<decltype(std::declval<NodePtr>()->*Next), NodePtr&>,
                  "IntrusiveList hooks must be members of type NodePtr");

public:
    IntrusiveList() = default;
    IntrusiveList(IntrusiveList &&other) :
        m_size(std::exchange(other.m_size, 0)),
        m_head(std::exchange(other.m_head, nullptr)),
        m_tail(std::exchange(other.m_tail, nullptr))
    {}
    IntrusiveList& operator=(IntrusiveList &&other)
    {
        clear();
        m_size = std::exchange(other.m_size, 0);
        m_head = std::exchange(other.m_head, nullptr);
        m_tail = std::exchange(other.m_tail, nullptr);
        return *this;
    }
    ~IntrusiveList()
    {
        clear();
    }

    void clear()
    {
        m_size = 0;
        while (auto node = m_head) {
            prev(m_head) = nullptr;
            m_head = next(node);
            next(node) = nullptr;
        }
        m_tail = nullptr;
    }

    /// @brief Returns true if the list contains no nodes.
    bool empty() const noexcept { return m_head == nullptr; }

    /// @brief Appends all nodes from @p other to the back of this list in O(1).
    ///
    /// @p other is left empty after the call. No nodes are copied or moved —
    /// the two lists' tail/head pointers are simply re-linked.
    void push_back_block(IntrusiveList &&other) {
        if (other.empty()) {
            return;
        } else if (empty()) {
            m_size =std::exchange(other.m_size, 0);
            m_head = std::exchange(other.m_head, nullptr);
            m_tail = std::exchange(other.m_tail, nullptr);
        } else {
            m_size += std::exchange(other.m_size, 0);
            prev(other.m_head) = m_tail;
            next(m_tail) = std::exchange(other.m_head, nullptr);
            m_tail = std::exchange(other.m_tail, nullptr);
        }
    }

    /// @brief Appends @p node to the back of the list.
    void push_back(NodePtr node) noexcept {
        prev(node) = m_tail;
        next(node) = nullptr;
        if (m_tail) next(m_tail) = node;
        else        m_head = node;
        m_tail = node;
        ++m_size;
    }

    /// @brief Removes up to @p n nodes from the back and returns them as a new list.
    ///
    /// Walks backward from the tail to find the split point — O(n). If @p n is
    /// greater than or equal to the current size, the entire list is transferred
    /// and this list is left empty. Returns an empty list if @p n is zero or this
    /// list is empty.
    IntrusiveList pop_back_block(std::size_t n)
    {
        IntrusiveList list;
        if (m_size == 0 || n == 0) {
            return list;
        }
        NodePtr node = m_tail;
        size_t i = 1;
        for (; i < n && prev(node) != nullptr; ++i) {
            node = prev(node);
        }
        if (node == m_head) {
            list.m_size = std::exchange(m_size, 0);
            list.m_head = std::exchange(m_head, nullptr);
            list.m_tail = std::exchange(m_tail, nullptr);
        } else {
            list.m_size = i;
            list.m_head = node;
            list.m_tail = m_tail;
            m_size -= i;
            m_tail = prev(node);
            prev(list.m_head) = nullptr;
            next(m_tail) = nullptr;
        }
        return list;
    }

    /// @brief Removes and returns the back node, or nullptr if empty.
    NodePtr pop_back() noexcept {
        if (!m_head) return nullptr;
        NodePtr node = m_tail;
        m_tail = prev(node);
        if (m_tail) next(m_tail) = nullptr;
        else        m_head = nullptr;
        prev(node) = next(node) = nullptr;
        --m_size;
        return node;
    }

    /// @brief Removes and returns the front node, or nullptr if empty.
    NodePtr pop_front() noexcept {
        if (!m_head) return nullptr;
        NodePtr node = m_head;
        m_head = next(node);
        if (m_head) prev(m_head) = nullptr;
        else        m_tail = nullptr;
        prev(node) = next(node) = nullptr;
        --m_size;
        return node;
    }

    /// @brief Unlinks @p node from wherever it sits in the list.
    ///
    /// Safe to call unconditionally on any node, whether or not it is
    /// currently linked into this list — a no-op if not. Nodes with null
    /// links are ambiguous (never-linked vs. sole element), so this
    /// is disambiguated via `m_head`, which only this list has access to;
    /// callers should not try to track linkage themselves and should always
    /// call `remove()` rather than guessing first.
    void remove(NodePtr node) noexcept {
        if (!prev(node) && !next(node) && m_head != node) return;
        if (prev(node)) next(prev(node)) = next(node);
        else            m_head = next(node);
        if (next(node)) prev(next(node)) = prev(node);
        else            m_tail = prev(node);
        --m_size;
        prev(node) = next(node) = nullptr;
    }

    inline std::size_t size() const noexcept { return m_size; }

    /// @brief The front node without removing it, or nullptr if empty. Walk the rest
    /// through the `Next` hook, under the same lock that guards the list.
    NodePtr front() const noexcept { return m_head; }
private:
    static NodePtr& prev(NodePtr node) noexcept { return node->*Prev; }
    static NodePtr& next(NodePtr node) noexcept { return node->*Next; }

    std::size_t m_size = 0;
    NodePtr m_head = nullptr;
    NodePtr m_tail = nullptr;
};

} // namespace coro::detail
