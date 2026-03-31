#pragma once

#include <cstddef>

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
 * All operations are O(1). Not thread-safe; callers must hold an external lock.
 */
class IntrusiveList {
public:
    /// @brief Returns true if the list contains no nodes.
    bool empty() const noexcept { return m_head == nullptr; }

    /// @brief Appends @p node to the back of the list.
    void push_back(IntrusiveListNode* node) noexcept {
        node->prev = m_tail;
        node->next = nullptr;
        if (m_tail) m_tail->next = node;
        else        m_head = node;
        m_tail = node;
    }

    /// @brief Removes and returns the front node, or nullptr if empty.
    IntrusiveListNode* pop_front() noexcept {
        if (!m_head) return nullptr;
        auto* node = m_head;
        m_head = node->next;
        if (m_head) m_head->prev = nullptr;
        else        m_tail = nullptr;
        node->prev = node->next = nullptr;
        return node;
    }

    /// @brief Unlinks @p node from wherever it sits in the list.
    void remove(IntrusiveListNode* node) noexcept {
        if (node->prev) node->prev->next = node->next;
        else            m_head = node->next;
        if (node->next) node->next->prev = node->prev;
        else            m_tail = node->prev;
        node->prev = node->next = nullptr;
    }

private:
    IntrusiveListNode* m_head = nullptr;
    IntrusiveListNode* m_tail = nullptr;
};

} // namespace coro::detail
