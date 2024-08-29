yambar-modules-mpris(5)

# NAME
mpris - This module provides MPRIS status such as currently playing artist/album/song

# TAGS

[[ *Name*
:[ *Type*
:< *Description*
|  state
:  string
:  One of *offline*, *stopped*, *paused* or *playing*
|  shuffle
:  bool
:  True if the *shuffle* flag is set
|  repeat
:  string
:  One of *none*, *track* or *paylist*
|  volume
:  range
:  Volume in percentage
|  album
:  string
:  Currently playing album
|  artist
:  string
:  Artist of currently playing song
|  title
:  string
:  Title of currently playing song
|  file
:  string
:  Filename or URL of currently playing song
|  pos
:  string
:  *%M:%S*-formatted string describing the song's current position
   (also see _elapsed_)
|  end
:  string
:  *%M:%S*-formatted string describing the song's total length (also
   see _duration_)
|  elapsed
:  realtime
:  Position in currently playing song, in milliseconds. Can be used
   with a _progress-bar_ particle.

# CONFIGURATION

[[ *Name*
:[ *Type*
:[ *Req*
:< *Description*
|  identity
:  string
:  yes
:  Identity if the target MPRIS client
|  query-timeout
:  int
:  no
:  Timeout for MPRIS clients to respond to queries. Defaults to 500

# EXAMPLES

```
bar:
	center:
		- mpris:
			identity: "spotify"
			content:
			  - map:
				  conditions:
					state != offline && state != stopped:
					  - list:
						  items:
							- string: {text: "{artist}", max: 30 }
							- string: {text: "-" }
							- string: {text: "{title}", max: 30 }
							- string: {text: "|" }
							- string: {text: "{pos} / {end}", max: 30 }
```

# SEE ALSO

*yambar-modules*(5), *yambar-particles*(5), *yambar-tags*(5), *yambar-decorations*(5)
