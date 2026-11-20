// WaveCue Set Planner - private, in-memory planning surface.
// Included inside gen_wavecue.cpp's anonymous namespace, after cache helpers.
// Intentionally NEVER writes to Winamp's playlist or to disk.
constexpr int SP_IMPORT=3101, SP_SORT=3102, SP_SORT_SEL=3103, SP_UP=3104,
 SP_DOWN=3105, SP_LOCK=3106, SP_PLUS2=3107, SP_MINUS2=3108,
 SP_PLUS7=3109, SP_MINUS7=3110, SP_END=3111, SP_UNDO=3112,
 SP_FIND=3113;
constexpr int SP_LIST=3190;
struct PlannerRow {
    int type=0; // 0 = song, 1 = transition, 2 = end of playlist
    std::wstring title, path, key;
    int bpm=-1, jump=0, originalIndex=-1;
    bool locked=false;
};
HWND spWindow=nullptr, spList=nullptr;
std::vector<PlannerRow> spRows, spUndo;
bool spHasUndo=false;
int spDragFrom=-1;

int SpNumber(const std::wstring& k) {
    if(k.empty()) return -1;
    wchar_t* end=nullptr;
    const long n=wcstol(k.c_str(),&end,10);
    return end!=k.c_str() && n>=1 && n<=12 ? (int)n : -1;
}
int SpAdvance(int number,int shift) {
    return ((number-1+shift%12+12)%12)+1;
}
int SpSelectedFirst() {
    return spList ? ListView_GetNextItem(spList,-1,LVNI_SELECTED) : -1;
}
void SpSnapshot() { spUndo=spRows; spHasUndo=true; }
bool SpInReserve(int pos) {
    for(int i=0;i<pos && i<(int)spRows.size();++i)
        if(spRows[i].type==2) return true;
    return false;
}
int SpQuality(int index) {
    if(index<0 || index>=(int)spRows.size() || spRows[index].type!=0) return -1;
    if(SpInReserve(index)) return -1;
    const int cur=SpNumber(spRows[index].key);
    if(cur<0) return -1;
    int p=index-1, jump=0;
    if(p>=0 && spRows[p].type==1) { jump=spRows[p].jump; --p; }
    while(p>=0 && spRows[p].type!=0) --p;
    if(p<0 || SpNumber(spRows[p].key)<0) return -1;
    const int previous=SpNumber(spRows[p].key);
    const int goal=SpAdvance(previous,jump);
    const bool sameLetter=spRows[p].key.back()==spRows[index].key.back();
    if(cur==goal && (sameLetter || jump==0)) return 2;
    if(jump==0 && sameLetter &&
       (cur==SpAdvance(previous,1) || cur==SpAdvance(previous,-1))) return 2;
    const int distance=std::min((cur-goal+12)%12,(goal-cur+12)%12);
    return distance<=1 ? 1 : 0;
}
void SpRender(int focus=-1) {
    if(!spList || !IsWindow(spList)) return;
    SendMessageW(spList,WM_SETREDRAW,FALSE,0);
    ListView_DeleteAllItems(spList);
    int i=0;
    for(const auto& row:spRows) {
        std::wstring title=row.type==2 ? L"=== END OF PLAYLIST / RESERVE BELOW ===" :
            row.type==1 ? (L"=== HARMONIC BREAK "+std::wstring(row.jump>0?L"+":L"")+
                             std::to_wstring(row.jump)+L" ===") : row.title;
        LVITEMW item{};
        item.mask=LVIF_TEXT; item.iItem=i; item.pszText=const_cast<wchar_t*>(title.c_str());
        ListView_InsertItem(spList,&item);
        std::wstring n=std::to_wstring(i+1);
        ListView_SetItemText(spList,i,1,const_cast<wchar_t*>(n.c_str()));
        std::wstring bpm=row.bpm>0?std::to_wstring(row.bpm):L"--";
        ListView_SetItemText(spList,i,2,const_cast<wchar_t*>(bpm.c_str()));
        ListView_SetItemText(spList,i,3,const_cast<wchar_t*>(row.key.c_str()));
        std::wstring status=row.type==2?L"RESERVE":row.type==1?L"BREAK":
          SpInReserve(i)?L"UNSORTED":SpQuality(i)==2?L"GREEN":
          SpQuality(i)==1?L"YELLOW":SpQuality(i)==0?L"RED":L"--";
        ListView_SetItemText(spList,i,4,const_cast<wchar_t*>(status.c_str()));
        std::wstring lock=row.locked?L"LOCKED":L"";
        ListView_SetItemText(spList,i,5,const_cast<wchar_t*>(lock.c_str()));
        ++i;
    }
    if(focus>=0 && focus<(int)spRows.size()) {
        ListView_SetItemState(spList,focus,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_EnsureVisible(spList,focus,FALSE);
    }
    SendMessageW(spList,WM_SETREDRAW,TRUE,0);
    InvalidateRect(spList,nullptr,TRUE);
}
void SpImport() {
    if(!g_winamp || !IsWindow(g_winamp)) return;
    SpSnapshot();
    spRows.clear();
    const int total=std::max(0,(int)SendMessageW(g_winamp,WM_WA_IPC,0,IPC_GETLISTLENGTH));
    for(int i=0;i<std::min(total,2000);++i) {
        PlannerRow row;
        row.path=PlaylistPath(i);
        row.originalIndex=i;
        auto* p=reinterpret_cast<const wchar_t*>(
          SendMessageW(g_winamp,WM_WA_IPC,(WPARAM)i,213 /* IPC_GETPLAYLISTTITLEW */));
        if(p) row.title=p;
        if(row.title.empty()) row.title=row.path.empty()?L"(Untitled)":row.path;
        FileStamp stamp{};
        if(!row.path.empty() && ReadFileStamp(row.path,stamp)) {
            Task task{row.path,stamp};
            if(auto cached=LoadCache(task)) {
                if(cached->harmonicKey>=0) row.key=harmonic::Camelot(cached->harmonicKey);
                if(cached->bpm>=80) {
                    row.bpm=cached->bpm;
                    if(cached->bpmDouble) {
                        row.bpm=cached->bpm>=110?cached->bpm/2:cached->bpm*2;
                    }
                }
            }
        }
        spRows.push_back(std::move(row));
    }
    // Unanalysed tracks are automatically put into the separate reserve.
    // The divider itself is always available to reposition manually.
    auto knownEnd=std::stable_partition(spRows.begin(),spRows.end(),
        [](const PlannerRow& r){return !r.key.empty();});
    if(knownEnd!=spRows.end()) {
        PlannerRow divider; divider.type=2;
        spRows.insert(knownEnd,divider);
    }
    SpRender();
}
// Locate a track in Winamp without changing its playback position.
// Winamp's PE window supports Home/End and arrow navigation just like its UI.
constexpr UINT_PTR SP_FIND_TIMER=3191;
int spFindTarget=-1,spFindLast=-2,spFindStalls=0,spFindTicks=0;
int spFindCount=0,spFindMoves=0;
bool spFindAtBeginning=false;
HWND spFindPe=nullptr;
void SpStopFind() {
    if(spWindow && IsWindow(spWindow)) KillTimer(spWindow,SP_FIND_TIMER);
    spFindTarget=-1;spFindPe=nullptr;spFindLast=-2;
    spFindCount=0;spFindMoves=0;spFindAtBeginning=false;
}
void SpSendKey(WORD vk) {
    INPUT pair[2]{};
    pair[0].type=INPUT_KEYBOARD;
    pair[0].ki.wVk=vk;
    pair[1]=pair[0];
    pair[1].ki.dwFlags=KEYEVENTF_KEYUP;
    SendInput(2,pair,sizeof(INPUT));
}
void SpFindTick() {
    if(spFindTarget<0 || !spFindPe || !IsWindow(spFindPe) ||
       !g_winamp || !IsWindow(g_winamp)) {SpStopFind();return;}
    // Bound navigation to the actual size of the playlist, not 40 or 70.
    // The additional ticks allow the old Winamp UI to process key events.
    if(++spFindTicks>std::max(80,spFindCount*12+80)) {
        SpStopFind();return;
    }
    const int current=(int)SendMessageW(g_winamp,WM_WA_IPC,
                           (WPARAM)-1,IPC_PLAYLIST_GET_NEXT_SELECTED);
    if(current==spFindTarget && spFindAtBeginning) {SpStopFind();return;}
    if(!spFindAtBeginning) {
        // Always reset the selection to the top on each new Find.
        if(current==0 && spFindMoves>0) {
            spFindAtBeginning=true;spFindLast=0;spFindStalls=0;
        } else {
            if(++spFindStalls>=5) {
                SpSendKey(VK_HOME);
                spFindStalls=0;
                ++spFindMoves;
            }
            return;
        }
    }
    // Search forward from item zero, a single Down event at a time.
    // Do not send PgDown: Winamp's classic playlist can skip entries.
    if(current>spFindTarget || current>=spFindCount) {
        SpStopFind();return;
    }
    if(current==spFindLast) {
        if(++spFindStalls<4) return; // wait for the previous key to register
    } else {
        spFindLast=current;spFindStalls=0;
    }
    if(spFindMoves>spFindCount+30) {SpStopFind();return;}
    SpSendKey(VK_DOWN);
    ++spFindMoves;
    spFindStalls=0;
}
void SpFindInWinamp() {
    const int selected=SpSelectedFirst();
    if(selected<0 || selected>=(int)spRows.size() ||
       spRows[selected].type!=0 || spRows[selected].path.empty()) return;
    if(!g_winamp || !IsWindow(g_winamp)) return;
    const auto& row=spRows[selected];
    const int count=std::max(0,(int)SendMessageW(g_winamp,WM_WA_IPC,0,IPC_GETLISTLENGTH));
    int match=-1;
    if(row.originalIndex>=0 && row.originalIndex<count &&
       _wcsicmp(PlaylistPath(row.originalIndex).c_str(),row.path.c_str())==0)
        match=row.originalIndex;
    else for(int i=0;i<count;++i)
        if(_wcsicmp(PlaylistPath(i).c_str(),row.path.c_str())==0) {match=i;break;}
    if(match<0) {
        MessageBoxW(spWindow,L"Track missing from Winamp. Refresh the Planner.",
            L"Set Planner - Find",MB_OK|MB_ICONINFORMATION);
        return;
    }
    HWND pe=reinterpret_cast<HWND>(
        SendMessageW(g_winamp,WM_WA_IPC,1,260 /* IPC_GETWND_PE */));
    if(!pe || !IsWindow(pe)) return;
    SpStopFind();
    spFindPe=pe;spFindTarget=match;spFindCount=count;spFindLast=-2;
    spFindStalls=0;spFindTicks=0;spFindMoves=0;spFindAtBeginning=false;
    ShowWindow(pe,SW_SHOW);
    HWND root=GetAncestor(pe,GA_ROOT);
    if(!root) root=pe;
    ShowWindow(root,SW_SHOW);
    SetForegroundWindow(root);
    BringWindowToTop(pe);
    SetActiveWindow(root);
    SetFocus(pe);
    // Timer runs on the same Winamp UI thread; each event is dispatched
    // before the next, unlike the previous long SendInput burst.
    SetTimer(spWindow,SP_FIND_TIMER,45,nullptr);
}

void SpMove(int direction) {
    int i=SpSelectedFirst(), j=i+direction;
    if(i<0 || j<0 || j>=(int)spRows.size() || spRows[i].locked || spRows[j].locked) return;
    SpSnapshot(); std::swap(spRows[i],spRows[j]); SpRender(j);
}
void SpInsert(int type,int jump=0) {
    int selected=SpSelectedFirst();
    if(selected<0) selected=(int)spRows.size()-1;
    if(type==2) {
        for(const auto& r:spRows) if(r.type==2) return;
    }
    SpSnapshot();
    PlannerRow row;row.type=type;row.jump=jump;
    spRows.insert(spRows.begin()+selected+1,std::move(row));
    SpRender(selected+1);
}
void SpSort(bool selectionOnly) {
    if(spRows.empty()) return;
    std::vector<bool> selection(spRows.size(),!selectionOnly);
    if(selectionOnly) {
        std::fill(selection.begin(),selection.end(),false);
        for(int i=-1;(i=ListView_GetNextItem(spList,i,LVNI_SELECTED))!=-1;)
            if(i<(int)selection.size()) selection[i]=true;
    }
    SpSnapshot();
    // Every locked row, transition and reserve marker forms a fixed boundary.
    // Free songs may be rearranged within each section, never across boundaries.
    int start=0, reference=1;
    while(start<(int)spRows.size()) {
        if(spRows[start].type==2) break; // reserve stays untouched
        if(spRows[start].type==1) {
            reference=SpAdvance(reference,spRows[start].jump); ++start; continue;
        }
        if(spRows[start].locked) {
            if(SpNumber(spRows[start].key)>0) reference=SpNumber(spRows[start].key);
            ++start; continue;
        }
        int finish=start;
        while(finish<(int)spRows.size() && spRows[finish].type==0 &&
              !spRows[finish].locked) ++finish;
        std::vector<int> slots;
        std::vector<PlannerRow> pool;
        for(int i=start;i<finish;++i) if(selection[i]) {
            slots.push_back(i);pool.push_back(spRows[i]);
        }
        std::stable_sort(pool.begin(),pool.end(),[reference](const PlannerRow& a,const PlannerRow& b){
            const int ka=SpNumber(a.key),kb=SpNumber(b.key);
            if(ka<0 || kb<0) return ka>=0 && kb<0;
            const int da=(ka-reference+12)%12,db=(kb-reference+12)%12;
            if(da!=db) return da<db;
            if(a.key!=b.key) return a.key<b.key;
            if(a.bpm<0 || b.bpm<0) return a.bpm>=0 && b.bpm<0;
            return a.bpm<b.bpm;
        });
        for(size_t j=0;j<slots.size();++j) spRows[slots[j]]=std::move(pool[j]);
        for(int i=start;i<finish;++i)
            if(SpNumber(spRows[i].key)>0) reference=SpNumber(spRows[i].key);
        start=finish;
    }
    SpRender();
}
LRESULT CALLBACK SpListKeyboard(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,
                                  UINT_PTR id,DWORD_PTR) {
    if(msg==WM_KEYDOWN && wp=='A' && (GetKeyState(VK_CONTROL)&0x8000)) {
        for(int i=0;i<(int)spRows.size();++i)
            ListView_SetItemState(hwnd,i,LVIS_SELECTED,LVIS_SELECTED);
        return 0;
    }
    if(msg==WM_NCDESTROY) RemoveWindowSubclass(hwnd,SpListKeyboard,id);
    return DefSubclassProc(hwnd,msg,wp,lp);
}
void SpAddButton(HWND hwnd,int id,const wchar_t* title,int x,int y,int w) {
    HWND b=CreateWindowExW(0,L"BUTTON",title,WS_CHILD|WS_VISIBLE|WS_TABSTOP,
        x,y,w,27,hwnd,reinterpret_cast<HMENU>((INT_PTR)id),g_plugin.hDllInstance,nullptr);
    if(b&&g_font) SendMessageW(b,WM_SETFONT,(WPARAM)g_font,TRUE);
}
LRESULT CALLBACK SpWndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        spList=CreateWindowExW(WS_EX_CLIENTEDGE,L"SysListView32",L"",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|LVS_REPORT|LVS_SHOWSELALWAYS,
            12,48,920,485,hwnd,(HMENU)(INT_PTR)SP_LIST,g_plugin.hDllInstance,nullptr);
        ListView_SetExtendedListViewStyle(spList,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        SetWindowSubclass(spList,SpListKeyboard,1,0);
        const wchar_t* labels[]={L"Track",L"#",L"BPM",L"Camelot",L"Transition",L"Lock"};
        const int widths[]={495,43,65,83,120,90};
        for(int j=0;j<6;++j) {
            LVCOLUMNW c{}; c.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
            c.pszText=const_cast<wchar_t*>(labels[j]);c.cx=widths[j];c.iSubItem=j;
            ListView_InsertColumn(spList,j,&c);
        }
        if(g_font) SendMessageW(spList,WM_SETFONT,(WPARAM)g_font,TRUE);
        SpAddButton(hwnd,SP_IMPORT,L"Import / Refresh",12,10,124);
        SpAddButton(hwnd,SP_SORT,L"Sort All",145,10,86);
        SpAddButton(hwnd,SP_SORT_SEL,L"Sort Selection",240,10,122);
        SpAddButton(hwnd,SP_UNDO,L"Undo",371,10,70);
        SpAddButton(hwnd,SP_FIND,L"Find in Winamp",458,10,135);
        SpAddButton(hwnd,SP_UP,L"Up",12,545,60);
        SpAddButton(hwnd,SP_DOWN,L"Down",78,545,60);
        SpAddButton(hwnd,SP_LOCK,L"Toggle Lock",146,545,106);
        SpAddButton(hwnd,SP_PLUS2,L"+2",263,545,58);
        SpAddButton(hwnd,SP_MINUS2,L"-2",326,545,58);
        SpAddButton(hwnd,SP_PLUS7,L"+7",389,545,58);
        SpAddButton(hwnd,SP_MINUS7,L"-7",452,545,58);
        SpAddButton(hwnd,SP_END,L"End of Playlist",520,545,137);
        HWND hint=CreateWindowExW(0,L"STATIC",
          L"Click the Lock column or double-click a row. Find locates songs in Winamp; no playback is triggered.",
          WS_CHILD|WS_VISIBLE,12,584,905,23,hwnd,nullptr,g_plugin.hDllInstance,nullptr);
        if(hint&&g_font)SendMessageW(hint,WM_SETFONT,(WPARAM)g_font,TRUE);
        return 0;
    }
    case WM_TIMER:
        if(wp==SP_FIND_TIMER) {SpFindTick();return 0;}
        break;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case SP_IMPORT: SpImport();break;
        case SP_FIND: SpFindInWinamp();break;
        case SP_SORT: SpSort(false);break;
        case SP_SORT_SEL: SpSort(true);break;
        case SP_UP: SpMove(-1);break;
        case SP_DOWN: SpMove(1);break;
        case SP_LOCK: {
            bool any=false;
            for(int i=-1;(i=ListView_GetNextItem(spList,i,LVNI_SELECTED))!=-1;)
                if(i<(int)spRows.size()) {if(!any)SpSnapshot();any=true;spRows[i].locked=!spRows[i].locked;}
            if(any)SpRender();break;
        }
        case SP_PLUS2:SpInsert(1,2);break;
        case SP_MINUS2:SpInsert(1,-2);break;
        case SP_PLUS7:SpInsert(1,7);break;
        case SP_MINUS7:SpInsert(1,-7);break;
        case SP_END:SpInsert(2);break;
        case SP_UNDO:if(spHasUndo){spRows=spUndo;spHasUndo=false;SpRender();}break;
        } return 0;
    case WM_LBUTTONUP: {
        if(spDragFrom>=0) {
            POINT pt{};GetCursorPos(&pt);ScreenToClient(spList,&pt);
            LVHITTESTINFO hit{};hit.pt=pt;
            int target=ListView_HitTest(spList,&hit);
            int source=spDragFrom;spDragFrom=-1;
            if(GetCapture()==hwnd) ReleaseCapture();
            if(target>=0 && source!=target && !spRows[source].locked) {
                bool blocked=false;
                for(int n=std::min(source,target);n<=std::max(source,target);++n)
                    if(n!=source && spRows[n].locked) blocked=true;
                if(!blocked) {
                    SpSnapshot();
                    PlannerRow moved=std::move(spRows[source]);
                    spRows.erase(spRows.begin()+source);
                    spRows.insert(spRows.begin()+target,std::move(moved));
                    SpRender(target);
                }
            }
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* hdr=reinterpret_cast<NMHDR*>(lp);
        if(hdr && hdr->hwndFrom==spList) {
            if(hdr->code==LVN_BEGINDRAG) {
                const auto* drag=reinterpret_cast<NMLISTVIEW*>(lp);
                if(drag->iItem>=0 && drag->iItem<(int)spRows.size()
                    && !spRows[drag->iItem].locked) {
                    spDragFrom=drag->iItem;SetCapture(hwnd);
                }
                return 0;
            }
            if(hdr->code==NM_CLICK) {
                auto* hit=reinterpret_cast<NMITEMACTIVATE*>(lp);
                if(hit->iItem>=0 && hit->iItem<(int)spRows.size() &&
                   hit->iSubItem==5) {
                    SpSnapshot();
                    spRows[hit->iItem].locked=!spRows[hit->iItem].locked;
                    SpRender(hit->iItem);
                    return 0;
                }
            }
            if(hdr->code==NM_DBLCLK) {
                auto* hit=reinterpret_cast<NMITEMACTIVATE*>(lp);
                if(hit->iItem>=0&&hit->iItem<(int)spRows.size()) {
                    SpSnapshot();spRows[hit->iItem].locked=!spRows[hit->iItem].locked;
                    SpRender(hit->iItem);
                } return 0;
            }
            if(hdr->code==NM_CUSTOMDRAW) {
                auto* draw=reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                if(draw->nmcd.dwDrawStage==CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if(draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT) {
                    int i=(int)draw->nmcd.dwItemSpec;
                    if(i>=0&&i<(int)spRows.size()) {
                        int q=SpQuality(i);
                        draw->clrTextBk=spRows[i].type==2?RGB(45,38,34):
                          spRows[i].type==1?RGB(44,46,29):RGB(24,30,31);
                        draw->clrText=spRows[i].locked?RGB(186,216,149):
                            q==2?RGB(133,224,120):q==1?RGB(240,209,99):
                            q==0?RGB(237,120,108):RGB(199,213,200);
                    }
                    return CDRF_NEWFONT;
                }
            }
        }
        break;
    }
    case WM_KEYDOWN:
        if((GetKeyState(VK_CONTROL)&0x8000) && (wp=='A')) {
            for(int i=0;i<(int)spRows.size();++i)
                ListView_SetItemState(spList,i,LVIS_SELECTED,LVIS_SELECTED);
            return 0;
        } break;
    case WM_CLOSE:SpStopFind();spDragFrom=-1; if(GetCapture()==hwnd)ReleaseCapture();ShowWindow(hwnd,SW_HIDE);return 0;
    case WM_DESTROY:SpStopFind();spList=nullptr;spWindow=nullptr;return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void ShowSetPlanner() {
    if(spWindow&&IsWindow(spWindow)) {
        ShowWindow(spWindow,SW_RESTORE);SetForegroundWindow(spWindow);return;
    }
    WNDCLASSEXW c{};c.cbSize=sizeof(c);c.lpfnWndProc=SpWndProc;
    c.hInstance=g_plugin.hDllInstance;c.lpszClassName=L"WaveCueSetPlanner1";
    c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    c.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassExW(&c);
    spWindow=CreateWindowExW(WS_EX_APPWINDOW,c.lpszClassName,
      L"WaveCue - Set Planner (Sandbox)",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
      CW_USEDEFAULT,CW_USEDEFAULT,958,650,g_hwnd,nullptr,g_plugin.hDllInstance,nullptr);
    if(spWindow) {
        ShowWindow(spWindow,SW_SHOW);UpdateWindow(spWindow);
        if(spRows.empty()) SpImport();else SpRender();
    }
}
