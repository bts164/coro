#pragma once

#include <cstddef>
#include <utility>

namespace coro::detail {

/**
 * @brief Base node for an intrusive doubly-linked list.
 *
 * Embed this (or inherit from it) in any struct that needs to participate in
 * an @ref IntrusiveList. The containing object owns its own storage — the list
 * holds raw non-owning pointers.
 *
 * A node must not be destroyed while it is still linked. Callers are
 * responsible for removing nodes before destruction (typically in a future's
 * destructor under the channel mutex).
 */
 struct IntrusiveListNode {
    IntrusiveListNode* prev = nullptr;
    IntrusiveListNode* next = nullptr;

    /// @brief Returns true if this node is currently in a list.
    bool is_linked() const noexcept { return prev != nullptr || next != nullptr; }
};

/**
 * @brief Intrusive doubly-linked list of @ref IntrusiveListNode objects.
 *
 * Does not own the nodes — all storage is in the embedded nodes themselves.
 * Not thread-safe; callers must hold an external lock.
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
 */
template<typename NodePtr = IntrusiveListNode*>
class IntrusiveList {
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
            m_head->prev = nullptr;
            m_head = node->next;
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
            other.m_head->prev = m_tail;
            m_tail->next = std::exchange(other.m_head, nullptr);
            m_tail = std::exchange(other.m_tail, nullptr);
        }
    }

    /// @brief Appends @p node to the back of the list.
    void push_back(NodePtr node) noexcept {
        node->prev = m_tail;
        node->next = nullptr;
        if (m_tail) m_tail->next = node;
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
        for (; i < n && node->prev != nullptr; ++i) {
            node = node->prev;
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
            m_tail = node->prev;
            list.m_head->prev = nullptr;
            m_tail->next = nullptr;
        }
        return list;
    }

    /// @brief Removes and returns the front node, or nullptr if empty.
    NodePtr pop_back() noexcept {
        if (!m_head) return nullptr;
        NodePtr node = m_tail;
        m_tail = node->prev;
        if (m_tail) m_tail->next = nullptr;
        else        m_head = nullptr;
        node->prev = node->next = nullptr;
        --m_size;
        return node;
    }

    /// @brief Removes and returns the front node, or nullptr if empty.
    NodePtr pop_front() noexcept {
        if (!m_head) return nullptr;
        NodePtr node = m_head;
        m_head = node->next;
        if (m_head) m_head->prev = nullptr;
        else        m_tail = nullptr;
        node->prev = node->next = nullptr;
        --m_size;
        return node;
    }

    /// @brief Unlinks @p node from wherever it sits in the list.
    void remove(NodePtr node) noexcept {
        if (node->prev) node->prev->next = node->next;
        else            m_head = node->next;
        if (node->next) node->next->prev = node->prev;
        else            m_tail = node->prev;
        --m_size;
        node->prev = node->next = nullptr;
    }

    inline std::size_t size() const noexcept { return m_size; }
private:
    std::size_t m_size = 0;
    NodePtr m_head = nullptr;
    NodePtr m_tail = nullptr;
};

} // namespace coro::detail
