	.section .rodata

	.include "asm/macros/m4a.inc"
	.include "asm/macros/music_voice.inc"

	.include "sound/voice_groups.inc"
	.include "sound/keysplit_tables.inc"
	.include "sound/programmable_wave_data.inc"
	.include "sound/music_player_table.inc"
	/* R12-E: the native (LINUX64) gSongTable rows must hold ROM logical
	 * addresses so HostResolveGbaAddr resolves every header into the audio
	 * arena; the GBA build keeps the original link-time emission. Both
	 * branches are inlined by preproc and gated by `as` (.if/.else/.endif),
	 * exactly like the voice_group macro's LINUX64 switch. */
	.if LINUX64
		.include "sound/song_table_native.generated.inc"
	.else
		.include "sound/song_table.inc"
	.endif
	.include "sound/direct_sound_data.inc"

	.align 2
