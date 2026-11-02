#ifndef __DPI_H__
#define __DPI_H__
#pragma once

#ifndef DPI_ENUMS_DECLARED

enum PROCESS_DPI_AWARENESS {
	PROCESS_DPI_UNAWARE = 0,
	PROCESS_SYSTEM_DPI_AWARE = 1,
	PROCESS_PER_MONITOR_DPI_AWARE = 2
};

enum MONITOR_DPI_TYPE {
	MDT_EFFECTIVE_DPI = 0,
	MDT_ANGULAR_DPI = 1,
	MDT_RAW_DPI = 2,
	MDT_DEFAULT = MDT_EFFECTIVE_DPI
};

#define DPI_ENUMS_DECLARED
#endif // (DPI_ENUMS_DECLARED)

namespace DuiLib
{
	class UILIB_API CDPI
	{
	public:
		CDPI(void);

	public:
		static int GetMainMonitorDPI();
		static int GetDPIOfMonitor(HMONITOR hMonitor);
		static int GetDPIOfMonitorNearestToPoint(POINT pt);
	public:
		// [WIN11-02] Opt into Per-Monitor V2 DPI awareness.
		//
		// V2 (Win10 1703+) fixes non-client-area scaling and mixed-DPI multi
		// monitor behaviour that V1 gets wrong, which matters for a self-drawn
		// title bar. Falls back V2 -> V1 -> System -> Unaware on older systems.
		//
		// Returns the awareness level actually set:
		//   3 = Per-Monitor V2, 2 = Per-Monitor V1, 1 = System, 0 = Unaware
		// Must be called before the first window is created.
		static int SetPerMonitorV2Awareness();


	public:
		PROCESS_DPI_AWARENESS GetDPIAwareness();
		BOOL SetDPIAwareness(PROCESS_DPI_AWARENESS Awareness);
		UINT GetDPI();
		UINT GetScale();
		void SetScale(UINT uDPI);
		RECT Scale(RECT rcRect);
		void Scale(RECT *pRect);
		POINT Scale(POINT ptPoint);
		void Scale(POINT *pPoint);
		SIZE Scale(SIZE szSize);
		void Scale(SIZE *pSize);
		int Scale(int iValue);

		int  ScaleBack(int iValue);
		void ScaleBack(RECT *pRect);

	private:
		int m_nScaleFactor;
		int m_nScaleFactorSDA;
		PROCESS_DPI_AWARENESS m_Awareness;
	};
}
#endif //__DPI_H__