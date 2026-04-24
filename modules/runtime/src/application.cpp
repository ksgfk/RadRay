#include <radray/runtime/application.h>

#include <radray/logger.h>
#include <radray/utility.h>

namespace radray {

AppWindow::~AppWindow() noexcept {
    this->ResetMailboxes();
}

void AppWindow::ResetMailboxes() noexcept {
    {
        std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
        if (_app->_multiThreaded) lock.lock();

        for (FlightData& flight : _flights) {
            if (flight._state == FlightState::InRender) {
                flight._task.Wait();
                flight._task = {};
            }
            flight._mailboxSlot = 0;
            flight._state = FlightState::Free;
        }
        for (MailboxData& mailbox : _mailboxes) {
            mailbox._state = MailboxState::Free;
        }
        _latestPublished.reset();
    }

    {
        std::unique_lock<std::mutex> lock{_channel._mutex, std::defer_lock};
        if (_app->_multiThreaded) lock.lock();
        _channel._queue.clear();
    }
}

std::optional<uint32_t> AppWindow::AllocMailboxSlot() noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    std::optional<uint32_t> selectedSlot;
    if (_latestPublished.has_value()) {
        const MailboxSnapshot last = *_latestPublished;
        RADRAY_ASSERT(last.Slot < _mailboxes.size());
        _latestPublished.reset();
        const MailboxData& mailbox = _mailboxes[last.Slot];
        RADRAY_ASSERT(mailbox._state == MailboxState::Published);
        selectedSlot = last.Slot;
    }
    if (!selectedSlot.has_value()) {
        for (size_t i = 0; i < _mailboxes.size(); i++) {
            if (_mailboxes[i]._state == MailboxState::Free) {
                selectedSlot = static_cast<uint32_t>(i);
                break;
            }
        }
    }
    if (!selectedSlot.has_value()) {
        return std::nullopt;
    }
    MailboxData& mailbox = _mailboxes[*selectedSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Free || mailbox._state == MailboxState::Published);
    mailbox._generation++;
    mailbox._state = MailboxState::Preparing;
    return selectedSlot;
}

void AppWindow::PublishPreparedMailbox(uint32_t mailboxSlot) noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());
    RADRAY_ASSERT(_mailboxes[mailboxSlot]._state == MailboxState::Preparing);
    if (_latestPublished.has_value()) {
        const MailboxSnapshot oldSnapshot = *_latestPublished;
        RADRAY_ASSERT(oldSnapshot.Slot < _mailboxes.size());
        _latestPublished.reset();
        if (oldSnapshot.Slot != mailboxSlot) {
            MailboxData& oldMailbox = _mailboxes[oldSnapshot.Slot];
            RADRAY_ASSERT(oldMailbox._state == MailboxState::Published);
            RADRAY_ASSERT(oldMailbox._generation == oldSnapshot.Generation);
            oldMailbox._state = MailboxState::Free;
        }
    }
    _mailboxes[mailboxSlot]._state = MailboxState::Published;
    _latestPublished = MailboxSnapshot{mailboxSlot, MailboxState::Published, _mailboxes[mailboxSlot]._generation};
}

void AppWindow::ReleaseMailbox(uint32_t mailboxSlot) noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());
    MailboxData& mailbox = _mailboxes[mailboxSlot];
    if (mailbox._state == MailboxState::Free) {
        return;
    }
    mailbox._state = MailboxState::Free;
    if (_latestPublished.has_value() && _latestPublished->Slot == mailboxSlot) {
        _latestPublished.reset();
    }
}

void AppWindow::CollectCompletedFlightSlots() noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    for (FlightData& flight : _flights) {
        if (flight._state != FlightState::InRender || !flight._task.IsCompleted()) {
            continue;
        }
        if (flight._state != FlightState::Free) {
            RADRAY_ASSERT(flight._mailboxSlot < _mailboxes.size());
            MailboxData& mailbox = _mailboxes[flight._mailboxSlot];
            if (mailbox._state != MailboxState::Free) {
                mailbox._state = MailboxState::Free;
            }
            if (_latestPublished.has_value() && _latestPublished->Slot == flight._mailboxSlot) {
                _latestPublished.reset();
            }
        }
        flight._task = {};
        flight._mailboxSlot = 0;
        flight._state = FlightState::Free;
    }
}

bool AppWindow::CanRender() const noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    if (_pendingRecreate) {
        return false;
    }
    if (!_window->IsValid() || _window->ShouldClose() || _window->IsMinimized()) {
        return false;
    }
    const WindowVec2i size = _window->GetSize();
    return size.X > 0 &&
           size.Y > 0 &&
           _surface->IsValid() &&
           !_flights.empty() &&
           !_mailboxes.empty();
}

}  // namespace radray
