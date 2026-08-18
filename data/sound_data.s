	.section .rodata

	.include "asm/macros/m4a.inc"
	.include "asm/macros/music_voice.inc"

	/* R12-G: on the 64-bit native linux link every migrated audio payload
	 * include is compiled out of this object - voicegroups, keysplits,
	 * programmable waves, the direct-sound samples and the 530 song objects
	 * are all removed from the native link and resolved from the audio arena
	 * at runtime instead (gSongTable native rows carry ROM logical addresses,
	 * R12-E). The .else branch below is the exact original include set in the
	 * exact original order, so the GBA build assembles byte-identical data
	 * (music_player_table.inc appears in both branches - gMPlayTable is a
	 * structural stay-compiled exception, R12-G §8). */
	.if (NATIVE_LINUX == 1) && (LINUX64 == 1)
		.include "sound/music_player_table.inc"
		/* R12-E: the native (LINUX64) gSongTable rows must hold ROM logical
		 * addresses so HostResolveGbaAddr resolves every header into the audio
		 * arena; the GBA build keeps the original link-time emission. Both
		 * branches are inlined by preproc and gated by `as` (.if/.else/.endif),
		 * exactly like the voice_group macro's LINUX64 switch. */
		.include "sound/song_table_native.generated.inc"
	.else
		.include "sound/voice_groups.inc"
		.include "sound/keysplit_tables.inc"
		.include "sound/programmable_wave_data.inc"
		.include "sound/music_player_table.inc"
		.include "sound/song_table.inc"
		.include "sound/direct_sound_data.inc"
	.endif

	.align 2
