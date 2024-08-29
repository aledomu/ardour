#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

class MutexTest : public CppUnit::TestFixture
{
	CPPUNIT_TEST_SUITE (MutexTest);
	CPPUNIT_TEST (testBasic);
	CPPUNIT_TEST_SUITE_END ();

public:
	MutexTest ();
	void testBasic ();

private:
	std::mutex m_mutex;
};
