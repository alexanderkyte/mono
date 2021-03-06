//
// System.Web.Configuration.HttpModuleAction
//
// Authors:
//	Chris Toshok (toshok@ximian.com)
//
// (C) 2005 Novell, Inc (http://www.novell.com)
//

//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//


using System;
using System.ComponentModel;
using System.Configuration;

namespace System.Web.Configuration
{
	public sealed class HttpModuleAction: ConfigurationElement
	{
		static ConfigurationPropertyCollection properties;
		static ConfigurationProperty nameProp;
		static ConfigurationProperty typeProp;

		static ConfigurationElementProperty elementProperty;

		static HttpModuleAction ()
		{
			nameProp = new ConfigurationProperty ("name", typeof (string), null,
							      TypeDescriptor.GetConverter (typeof (string)),
							      PropertyHelper.NonEmptyStringValidator,
							      ConfigurationPropertyOptions.IsRequired | ConfigurationPropertyOptions.IsKey);
			typeProp = new ConfigurationProperty ("type", typeof (string), "hoho", ConfigurationPropertyOptions.IsRequired);
			properties = new ConfigurationPropertyCollection ();
			properties.Add (nameProp);
			properties.Add (typeProp);

			elementProperty = new ConfigurationElementProperty (new CallbackValidator (typeof (HttpModuleAction), ValidateElement));
		}

		internal HttpModuleAction ()
		{
		}

		public HttpModuleAction (string name, string type)
		{
			this.Name = name;
			this.Type = type;
		}

		static void ValidateElement (object o)
		{
			/* XXX do some sort of element validation here? */
		}

		protected internal override ConfigurationElementProperty ElementProperty {
			get { return elementProperty; }
		}

		[StringValidator (MinLength = 1)]
		[ConfigurationProperty ("name", DefaultValue = "", Options = ConfigurationPropertyOptions.IsRequired | ConfigurationPropertyOptions.IsKey)]
		public string Name {
			get { return (string)base[nameProp]; }
			set { base[nameProp] = value; }
		}

		[ConfigurationProperty ("type", DefaultValue = "", Options = ConfigurationPropertyOptions.IsRequired)]
		public string Type {
			get { return (string)base[typeProp]; }
			set { base[typeProp] = value; }
		}

		protected internal override ConfigurationPropertyCollection Properties {
			get { return properties; }
		}
	}
}

