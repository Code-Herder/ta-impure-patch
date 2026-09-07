// smokeunit.h — the damage-smoke thread every stock unit starts from Create. This text
// compiles to the exact words of the stock SmokeUnit (tools/tacob dump armstump SmokeUnit),
// which is why it is written with the bare numbers Cavedog used. Needs a piece named base.
SmokeUnit(healthpercent, sleeptime, smoketype)
{
	while( get BUILD_PERCENT_LEFT )
	{
		sleep 400;
	}
	while( TRUE )
	{
		healthpercent = get HEALTH;
		if( healthpercent < 66 )
		{
			smoketype = 256 | 2;
			if( rand( 1, 66 ) < healthpercent )
			{
				smoketype = 256 | 1;
			}
			emit-sfx smoketype from base;
		}
		sleeptime = healthpercent * 50;
		if( sleeptime < 200 )
		{
			sleeptime = 200;
		}
		sleep sleeptime;
	}
}
