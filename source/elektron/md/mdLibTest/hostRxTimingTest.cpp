#include "mdLib/mdtimedhostrx.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
	void require(const bool _condition, const char* _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}
}

int main()
{
	try
	{
		md::TimedHostRx latch;
		uint32_t received = 0xaabbcc;
		require(!latch.take(1000, received), "empty latch supplied a word");
		require(received == 0xaabbcc, "empty read changed its destination");

		// The producer runs ahead and fills the host latch. Its receive request
		// must not become visible before the host reaches that machine time.
		require(latch.stage(0, 2000), "could not stage the first notification word");
		require(latch.pending(), "future word did not reserve the host latch");
		require(!latch.take(1000, received), "notification escaped from the future");
		require(!latch.take(1999, received), "notification arrived a cycle early");
		require(!latch.stage(0x41f, 2002), "second word overwrote the reserved latch");
		require(latch.take(2000, received) && received == 0,
			"first word was lost or delayed beyond its timestamp");
		require(!latch.pending(), "consumed word left the latch reserved");
		require(!latch.take(2000, received), "notification was delivered twice");

		// The second word waits in HOTX until the first leaves the host latch.
		// When transferred, retain its production timestamp rather than inventing
		// another delay based on when the host finally consumes the first word.
		require(latch.stage(0x41f, 2002), "could not stage the clock word");
		require(!latch.take(2001, received), "clock word arrived before production");
		require(latch.take(2010, received) && received == 0x41f,
			"late host did not receive the clock word in order");

		// Long sessions must preserve the full timestamp rather than truncating
		// the CPU cycle count to 32 bits or treating cycle zero as no pending data.
		constexpr uint64_t lateCycle = (uint64_t{1} << 40) + 7;
		require(latch.stage(0x123456, lateCycle), "could not stage long-session word");
		require(!latch.take(7, received), "ready timestamp was truncated");
		require(latch.take(lateCycle, received) && received == 0x123456,
			"long-session delivery failed");
		require(latch.stage(0xffffff, 0), "could not stage an immediately ready word");
		require(latch.take(0, received) && received == 0xffffff,
			"cycle-zero delivery failed");
		std::cout << "Timed host receive: early visibility, capacity, ordering and 64-bit deadlines passed\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
