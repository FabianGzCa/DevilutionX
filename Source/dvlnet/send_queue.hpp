#pragma once

#include <deque>
#include <memory>

#include "dvlnet/limits.hpp"
#include "dvlnet/packet.h"

namespace devilution::net {

/** Frames stay alive until their asynchronous write completes, even after Clear. */
class send_queue {
public:
	std::expected<void, PacketError> Push(buffer_t frame)
	{
		if (frames_.size() >= MaxQueuedPackets || frame.size() > MaxQueuedBytes - bytes_)
			return std::unexpected("Network send queue is full");
		bytes_ += frame.size();
		frames_.push_back(std::make_shared<buffer_t>(std::move(frame)));
		return {};
	}

	bool Empty() const { return frames_.empty(); }
	std::shared_ptr<const buffer_t> Front() const { return frames_.front(); }
	void Pop()
	{
		bytes_ -= frames_.front()->size();
		frames_.pop_front();
	}
	void Clear()
	{
		frames_.clear();
		bytes_ = 0;
	}

private:
	std::deque<std::shared_ptr<buffer_t>> frames_;
	size_t bytes_ = 0;
};

} // namespace devilution::net
