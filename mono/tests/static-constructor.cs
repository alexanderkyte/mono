
using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

public class StaticInitFails {
	public static string s;

	static StaticInitFails () {
		s = "FOO";
		throw new Exception ();
	}

	[MethodImpl(MethodImplOptions.NoInlining)]
	public static void foo () {
		// Console.WriteLine ("Foo");
	}
}

public class Tests {
	public static int Main() {
		try {
			StaticInitFails.foo ();
			return 1;
		}
		catch (Exception ex) {
			Console.WriteLine ("Success");
			return 0;
		}
	}
}
