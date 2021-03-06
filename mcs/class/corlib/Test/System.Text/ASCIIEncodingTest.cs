// ASCIIEncodingTest - NUnit Test Cases for the System.Text.ASCIIEncoding class
// 
// Author: Mike Kestner <mkestner@speakeasy.net>
//
// <c> 2002 Mike Kestner

using System;
using System.Collections.Generic;
using System.Text;

using NUnit.Framework;
using NUnit.Framework.Constraints;

namespace MonoTests.System.Text
{
	//
	// NOTE: when adding/updating tests here consider updating
	//       the following files as well since they have similar tests:
	//
	// - mcs/class/corlib/Test/System.Text/Latin1EncodingTest.cs
	// - mcs/class/I18N/EncodingTestBase.cs
	//
	[TestFixture]
	public class ASCIIEncodingTest
	{
		private char[] testchars;
		private byte[] testbytes;

		[SetUp]
		public void SetUp ()
		{
			testchars = new char[4];
			testchars[0] = 'T';
			testchars[1] = 'e';
			testchars[2] = 's';
			testchars[3] = 't';
			testbytes = new byte[4];
			testbytes[0] = (byte) 'T';
			testbytes[1] = (byte) 'e';
			testbytes[2] = (byte) 's';
			testbytes[3] = (byte) 't';
		}

		[Test]
		public void IsBrowserDisplay ()
		{
			Assert.IsFalse (Encoding.ASCII.IsBrowserDisplay);
		}

		[Test]
		public void IsBrowserSave ()
		{
			Assert.IsFalse (Encoding.ASCII.IsBrowserSave);
		}

		[Test]
		public void IsMailNewsDisplay ()
		{
			Assert.IsTrue (Encoding.ASCII.IsMailNewsDisplay);
		}

		[Test]
		public void IsMailNewsSave ()
		{
			Assert.IsTrue (Encoding.ASCII.IsMailNewsSave);
		}

		[Test] // Test GetBytes(char[])
		public void TestGetBytes1 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			byte[] bytes = ascii_encoding.GetBytes(testchars);
			for (int i = 0; i < testchars.Length; i++)
				Assert.AreEqual (testchars[i], (char) bytes[i]);
		}

		[Test] // Test GetBytes(char[], int, int)
		public void TestGetBytes2 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			byte[] bytes = ascii_encoding.GetBytes(testchars, 1, 1);
			Assert.AreEqual (1, bytes.Length, "#1");
			Assert.AreEqual (testchars [1], (char) bytes [0], "#2");
		}

		[Test] // Test non-ASCII char in char[]
		public void TestGetBytes3 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			testchars[2] = (char) 0x80;
			byte[] bytes = ascii_encoding.GetBytes(testchars);
			Assert.AreEqual ('T', (char) bytes [0], "#1");
			Assert.AreEqual ('e', (char) bytes [1], "#2");
			Assert.AreEqual ('?', (char) bytes [2], "#3");
			Assert.AreEqual ('t', (char) bytes [3], "#4");
		}

		[Test] // Test GetBytes(char[], int, int, byte[], int)
		public void TestGetBytes4 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			byte[] bytes = new Byte[1];
			int cnt = ascii_encoding.GetBytes(testchars, 1, 1, bytes, 0);
			Assert.AreEqual (1, cnt, "#1");
			Assert.AreEqual (testchars [1], (char) bytes [0], "#2");
		}

		[Test] // Test GetBytes(string, int, int, byte[], int)
		public void TestGetBytes5 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			byte[] bytes = new Byte[1];
			int cnt = ascii_encoding.GetBytes("Test", 1, 1, bytes, 0);
			Assert.AreEqual ('e', (char) bytes [0], "#1");
		}

		[Test] // Test GetBytes(string)
		public void TestGetBytes6 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			byte[] bytes = ascii_encoding.GetBytes("Test");
			for (int i = 0; i < testchars.Length; i++)
				Assert.AreEqual (testchars [i], (char) bytes [i]);
		}

		[Test] // Test GetBytes(string)
		public void TestGetBytes7 ()
		{
			var latin1_encoding = Encoding.GetEncoding ("latin1");

			var expected = new byte [] { 0x3F, 0x20, 0x3F, 0x20, 0x3F };
			var actual = latin1_encoding.GetBytes("\u24c8 \u2075 \u221e"); // normal replacement
			Assert.AreEqual (expected, actual, "#1");

			expected = new byte [] { 0x3F, 0x3F };
			actual = latin1_encoding.GetBytes("\ud83d\ude0a"); // surrogate pair replacement
			Assert.AreEqual (expected, actual, "#2");

			expected = new byte [] { 0x3F, 0x3F, 0x20 };
			actual = latin1_encoding.GetBytes("\ud83d\ude0a "); // surrogate pair replacement
			Assert.AreEqual (expected, actual, "#3");

			expected = new byte [] { 0x20, 0x20, 0x3F, 0x3F, 0x20, 0x20 };
			actual = latin1_encoding.GetBytes("  \ud83d\ude0a  "); // surrogate pair replacement
			Assert.AreEqual (expected, actual, "#4");

			expected = new byte [] { 0x20, 0x20, 0x3F, 0x3F, 0x20, 0x20 };
			actual = latin1_encoding.GetBytes("  \ud834\udd1e  "); // surrogate pair replacement
			Assert.AreEqual (expected, actual, "#5");

			expected = new byte [] { 0x41, 0x42, 0x43, 0x00, 0x41, 0x42, 0x43 };
			actual = latin1_encoding.GetBytes("ABC\0ABC"); // embedded zero byte not replaced
			Assert.AreEqual (expected, actual, "#6");

			expected = new byte [] { 0x20, 0x20, 0x3F, 0x20, 0x20 };
			actual = latin1_encoding.GetBytes("  \ud834  "); // invalid surrogate pair replacement
			Assert.AreEqual (expected, actual, "#7");
		}

		[Test] // Test GetChars(byte[])
		public void TestGetChars1 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			char[] chars = ascii_encoding.GetChars(testbytes);
			for (int i = 0; i < testbytes.Length; i++)
				Assert.AreEqual (testbytes[i], (byte) chars[i]);
		}

		[Test] // Test GetChars(byte[], int, int)
		public void TestGetChars2 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			char[] chars = ascii_encoding.GetChars(testbytes, 1, 1);
			Assert.AreEqual (1, chars.Length, "#1");
			Assert.AreEqual (testbytes [1], (byte) chars [0], "#2");
		}

		[Test] // Test non-ASCII char in byte[]
		public void TestGetChars3 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			testbytes[2] = 0x80;
			char[] chars = ascii_encoding.GetChars(testbytes);
			Assert.AreEqual ('T', chars [0], "#1");
			Assert.AreEqual ('e', chars [1], "#2");
			Assert.AreEqual ('?', chars [2], "#3");
			Assert.AreEqual ('t', chars [3], "#4");
		}

		[Test] // Test GetChars(byte[], int, int, char[], int)
		public void TestGetChars4 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			char[] chars = new char[1];
			int cnt = ascii_encoding.GetChars(testbytes, 1, 1, chars, 0);
			Assert.AreEqual (1, cnt, "#1");
			Assert.AreEqual (testbytes [1], (byte) chars [0], "#2");
		}

		[Test] // Test GetString(char[])
		public void TestGetString1 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			string str = ascii_encoding.GetString(testbytes);
			Assert.AreEqual ("Test", str);
		}

		[Test] // Test GetString(char[], int, int)
		public void TestGetString2 () 
		{
			Encoding ascii_encoding = Encoding.ASCII;
			string str = ascii_encoding.GetString(testbytes, 1, 2);
			Assert.AreEqual ("es", str);
		}

		[Test] // Test invalid byte handling
		public void TestGetString3 () 
		{
			Encoding encoding = Encoding.ASCII;
			byte [] bytes = new byte [] {0x61, 0xE1, 0xE2};
			string s = encoding.GetString (bytes, 0, 3);
			Assert.AreEqual ("a??", s);
		}

		[Test] // Test Decoder
		public void TestDecoder ()
		{
			Encoding ascii_encoding = Encoding.ASCII;
			char[] chars = new char[1];
			int cnt = ascii_encoding.GetDecoder().GetChars(testbytes, 1, 1, chars, 0);
			Assert.AreEqual (1, cnt, "#1");
			Assert.AreEqual (testbytes [1], (byte) chars [0], "#2");
		}

		[Test] // Test Decoder
		public void TestEncoder ()
		{
			Encoding ascii_encoding = Encoding.ASCII;
			byte[] bytes = new Byte[1];
			int cnt = ascii_encoding.GetEncoder().GetBytes(testchars, 1, 1, bytes, 0, false);
			Assert.AreEqual (1, cnt, "#1");
			Assert.AreEqual (testchars [1], (char) bytes [0], "#2");
		}

		[Test]
		public void TestZero ()
		{
			Encoding encoding = Encoding.ASCII;
			Assert.AreEqual (string.Empty, encoding.GetString (new byte [0]), "#1");
			Assert.AreEqual (string.Empty, encoding.GetString (new byte [0], 0, 0), "#2");
		}

		[Test]
		[ExpectedException (typeof (EncoderFallbackException))]
		public void EncoderFallback ()
		{
			Encoding e = Encoding.ASCII.Clone () as Encoding;
			e.EncoderFallback = new EncoderExceptionFallback ();
			e.GetBytes ("\u24c8");
		}

		[Test]
		public void EncoderFallback2 ()
		{
			Encoding e = Encoding.ASCII.Clone () as Encoding;
			e.EncoderFallback = new BackslashEncoderReplaceFallback ();

			byte[] bytes = e.GetBytes ("a\xac\u1234\u20ac\u8000");
			var expected = new byte[] { 0x61, 0x5C, 0x78, 0x61, 0x63, 0x5C, 0x75, 0x31, 0x32, 0x33, 0x34, 0x5C, 0x75, 0x32, 0x30, 0x61, 0x63, 0x5C, 0x75, 0x38, 0x30, 0x30, 0x30 };
			Assert.AreEqual (expected, bytes);

			bytes = e.GetBytes ("1\u04d92");
			expected = new byte[] { 0x31, 0x5C, 0x75, 0x30, 0x34, 0x64, 0x39, 0x32 };
			Assert.AreEqual (expected, bytes);

			e.EncoderFallback = new EncoderExceptionOnWrongIndexFallback ('\u04d9', 1);
			bytes = e.GetBytes ("1\u04d92");
			expected = new byte[] { 0x31, 0x21, 0x32 };
			Assert.AreEqual (expected, bytes);

			e.EncoderFallback = new EncoderExceptionOnWrongIndexFallback ('\u04d9', 0);
			bytes = e.GetBytes ("\u04d921");
			expected = new byte[] { 0x21, 0x32, 0x31 };
			Assert.AreEqual (expected, bytes);
		}

		[Test]
		[ExpectedException (typeof (DecoderFallbackException))]
		public void DecoderFallback ()
		{
			Encoding e = Encoding.ASCII.Clone () as Encoding;
			e.DecoderFallback = new DecoderExceptionFallback ();
			e.GetChars (new byte [] {0x80});
		}

		[Test]
	//	[ExpectedException (typeof (ArgumentException))]
		public void DecoderFallback2 ()
		{
			var bytes = new byte[] {
				0x30, 0xa0, 0x31, 0xa8
			};
			var enc = (ASCIIEncoding)Encoding.ASCII.Clone ();
			enc.DecoderFallback = new TestFallbackDecoder ();
			
			var chars = new char [7];
			var ret = enc.GetChars (bytes, 0, bytes.Length, chars, 0);
		}
		
		[Test]
		public void DecoderFallback3 ()
		{
			var bytes = new byte[] {
				0x30, 0xa0, 0x31, 0xa8
			};
			var enc = (ASCIIEncoding)Encoding.ASCII.Clone ();
			enc.DecoderFallback = new TestFallbackDecoder ();
			
			var chars = new char[] { '9', '8', '7', '6', '5' };
			var ret = enc.GetChars (bytes, 0, bytes.Length, chars, 0);
			
			Assert.That (ret, Is.EqualTo (2), "ret");
			Assert.That (chars [0], Is.EqualTo ('0'), "chars[0]");
			Assert.That (chars [1], Is.EqualTo ('1'), "chars[1]");
			Assert.That (chars [2], Is.EqualTo ('7'), "chars[2]");
			Assert.That (chars [3], Is.EqualTo ('6'), "chars[3]");
			Assert.That (chars [4], Is.EqualTo ('5'), "chars[4]");
		}
		
		class TestFallbackDecoder : DecoderFallback {
			const int count = 2;
			
			public override int MaxCharCount {
				get { return count; }
			}
			
			public override DecoderFallbackBuffer CreateFallbackBuffer ()
			{
				return new Buffer ();
			}
			
			class Buffer : DecoderFallbackBuffer {
				char[] queue;
				int index;
				
				public override int Remaining {
					get {
						return queue.Length - index;
					}
				}
				
				public override char GetNextChar ()
				{
					return index < queue.Length ? queue [index++] : '\0';
				}
				
				public override bool Fallback (byte[] bytes, int unused)
				{
					queue = new char[bytes.Length * count];
					index = 0;
					for (int i = 0; i < bytes.Length; i++) {
						for (int j = 0; j < count; j++)
							queue [index++] = (char)(bytes [i]+j);
					}
					return true;
				}
				
				public override bool MovePrevious ()
				{
					throw new NotImplementedException ();
				}
				
				public override void Reset ()
				{
					base.Reset ();
				}
			}
		}

		class BackslashEncoderReplaceFallback : EncoderFallback
		{
			class BackslashReplaceFallbackBuffer : EncoderFallbackBuffer
			{
				List<char> _buffer = new List<char> ();
				int _index;

				public override bool Fallback (char charUnknownHigh, char charUnknownLow, int index)
				{
					throw new NotImplementedException ();
					return false;
				}

				public override bool Fallback (char charUnknown, int index)
				{
					_buffer.Add('\\');
					int val = (int)charUnknown;
					if (val > 0xFF) {
						_buffer.Add ('u');
						AddCharacter (val >> 8);
						AddCharacter (val & 0xFF);
					} else {
						_buffer.Add ('x');
						AddCharacter (charUnknown);
					}
					return true;
				}

				private void AddCharacter (int val)
				{
					AddOneDigit (((val) & 0xF0) >> 4);
					AddOneDigit (val & 0x0F);
				}

				private void AddOneDigit (int val)
				{
					if (val > 9) {
						_buffer.Add ((char)('a' + val - 0x0A));
					} else {
						_buffer.Add ((char)('0' + val));
					}
				}

				public override char GetNextChar ()
				{
					if (_index == _buffer.Count)
						return Char.MinValue;

					return _buffer[_index++];
				}

				public override bool MovePrevious ()
				{
					if (_index > 0){
						_index--;
						return true;
					}
					return false;
				}

				public override int Remaining
				{
					get { return _buffer.Count - _index; }
				}
			}

			public override EncoderFallbackBuffer CreateFallbackBuffer ()
			{
				return new BackslashReplaceFallbackBuffer ();
			}

			public override int MaxCharCount
			{
				get { throw new NotImplementedException (); }
			}
		}

		class EncoderExceptionOnWrongIndexFallback : EncoderFallback
		{
			char _expectedCharUnknown;
			int _expectedIndex;

			public EncoderExceptionOnWrongIndexFallback (char expectedCharUnknown, int expectedIndex)
			{
				_expectedCharUnknown = expectedCharUnknown;
				_expectedIndex = expectedIndex;
			}

			public override EncoderFallbackBuffer CreateFallbackBuffer ()
			{
				return new EncoderExceptionOnWrongIndexFallbackBuffer (_expectedCharUnknown, _expectedIndex);
			}

			public override int MaxCharCount => 1;

			class EncoderExceptionOnWrongIndexFallbackBuffer : EncoderFallbackBuffer
			{
				char _expectedCharUnknown;
				int _expectedIndex;
				bool read;

				public EncoderExceptionOnWrongIndexFallbackBuffer (char expectedCharUnknown, int expectedIndex)
				{
					_expectedCharUnknown = expectedCharUnknown;
					_expectedIndex = expectedIndex;
				}

				public override int Remaining => read ? 0 : 1;

				public override bool Fallback (char charUnknown, int index)
				{
					Assert.AreEqual (_expectedCharUnknown, charUnknown);
					Assert.AreEqual (_expectedIndex, index);
					return true;
				}

				public override bool Fallback (char charUnknownHigh, char charUnknownLow, int index)
				{
					throw new NotImplementedException ();
					return true;
				}

				public override char GetNextChar ()
				{
					if (!read) {
						read = true;
						return '!';
					}
					return '\0';
				}

				public override bool MovePrevious ()
				{
					return false;
				}
			}
		}
	}
}
