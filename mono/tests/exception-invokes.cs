using System;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Reflection;

class C
{
	const int StepSize = 4;
	const int Iterations = 6;

	public static void Main ()
	{
		AppDomain.CurrentDomain.UnhandledException += new UnhandledExceptionEventHandler(HandleException);
		var args  = new object[] {C.Iterations * C.StepSize};
		typeof (C).GetMethod ("InvokeChain", BindingFlags.NonPublic | BindingFlags.Instance).Invoke (new C (), args);
	}

	public static void HandleException (object sender, UnhandledExceptionEventArgs e)
	{
		var ex = e.ExceptionObject as Exception;

		int iterations = 0;
		while (ex != null) {
			string fullTrace = ex.StackTrace;
			string[] frames = fullTrace.Split(new string[] { Environment.NewLine }, StringSplitOptions.None);

			// Console.WriteLine ("{0} frames", frames.Length);
			// foreach (var str in frames)
			// 	Console.WriteLine ("line {0}", str);
			frames = frames.Where (l => l.StartsWith ("  at C.InvokeChain")).ToArray ();
			// Console.WriteLine ("Post filter {0} frames", frames.Length);

			if (frames.Length != 0) {
				if (frames.Length != (C.StepSize-1)) {
					Console.WriteLine ("Fail step size");
					Environment.Exit (1);
				}
				iterations += 1;
			}

			ex = ex.InnerException;
		}

		if (iterations != C.Iterations) {
			Console.WriteLine ("{0} iterations", iterations);
			Environment.Exit (1);
		}

		// Prevents the runtime from printing the exception
		Environment.Exit (0);
	}


	[MethodImpl(MethodImplOptions.NoInlining)]
	private void InvokeChain (int depth)
	{
		if (depth == 0) {
			Base ();
		} else if (depth % C.StepSize == 0) {
			typeof (C).GetMethod ("InvokeChain", BindingFlags.NonPublic | BindingFlags.Instance).Invoke (this, new object[] {depth - 1});
		} else {
			InvokeChain (depth - 1);
		}
	}

	[MethodImpl(MethodImplOptions.NoInlining)]
	private void Base ()
	{
		throw new NotImplementedException ();
	}
}
