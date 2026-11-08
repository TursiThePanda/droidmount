/* Two-level exclusive lock for the MTP device.
 *
 * MTP is strictly one transaction at a time, so the device needs an exclusive lock. With
 * a multi-threaded FUSE loop, Finder/Quick Look thumbnail reads would otherwise queue in
 * front of the folder listing the user just clicked. High-priority holders (listings,
 * metadata, changes) are let in before any waiting low-priority holder (file reads and
 * writes); reads are cut into small chunks, so a listing waits at most one chunk.
 *
 * Not recursive. Low-priority waiters can starve while listings keep coming, which is
 * the intended trade-off (listings are short).
 */
#ifndef SMTPFS_PRIO_LOCK_H
#define SMTPFS_PRIO_LOCK_H

#include <condition_variable>
#include <mutex>

class PriorityLock
{
public:
    void lockHigh()
    {
        std::unique_lock<std::mutex> l(m_mutex);
        ++m_high_waiting;
        try {
            m_cv.wait(l, [this] { return !m_busy; });
        } catch (...) {
            --m_high_waiting;
            m_cv.notify_all();
            throw;
        }
        --m_high_waiting;
        m_busy = true;
    }

    void lockLow()
    {
        std::unique_lock<std::mutex> l(m_mutex);
        m_cv.wait(l, [this] { return !m_busy && m_high_waiting == 0; });
        m_busy = true;
    }

    void unlock()
    {
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_busy = false;
        }
        m_cv.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_busy = false;
    int m_high_waiting = 0;
};

class PriorityLockGuard
{
public:
    enum Level { High, Low };
    PriorityLockGuard(PriorityLock &lock, Level level): m_lock(lock)
    {
        if (level == High)
            m_lock.lockHigh();
        else
            m_lock.lockLow();
    }
    ~PriorityLockGuard() { m_lock.unlock(); }
    PriorityLockGuard(const PriorityLockGuard &) = delete;
    PriorityLockGuard &operator=(const PriorityLockGuard &) = delete;

private:
    PriorityLock &m_lock;
};

#endif // SMTPFS_PRIO_LOCK_H
