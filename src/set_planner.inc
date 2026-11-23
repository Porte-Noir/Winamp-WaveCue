// WaveCue Set Planner - private, in-memory planning surface.
// Included inside gen_wavecue.cpp's anonymous namespace, after cache helpers.
// Planning is private until Export to Winamp is explicitly confirmed.
constexpr int SP_IMPORT=3101, SP_SORT=3102, SP_SORT_SEL=3103, SP_UP=3104,
 SP_DOWN=3105, SP_REMOVE=3106, SP_PLUS2=3107, SP_MINUS2=3108,
 SP_PLUS7=3109, SP_MINUS7=3110, SP_END=3111, SP_UNDO=3112,
 SP_FIND=3113, SP_EXPORT=3114, SP_GOOFY=3115;
constexpr int SP_LIST=3190;
struct PlannerRow {
    int type=0; // 0 = song, 1 = transition, 2 = end of playlist
    std::wstring title, path, key;
    int bpm=-1, jump=0, originalIndex=-1;
    bool locked=false;
};
HWND spWindow=nullptr, spList=nullptr, spTrackNumber=nullptr;
constexpr UINT_PTR SP_NUMBER_TIMER=3192;
std::vector<PlannerRow> spRows, spUndo;
bool spHasUndo=false;
bool spGoofy=false;
HFONT spBoldFont=nullptr;
bool spRendering=false;
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
int SpHarmonicQuality(int index) {
    if(index<0 || index>=(int)spRows.size() || spRows[index].type!=0) return -1;
    if(SpInReserve(index)) return -1;
    const int cur=SpNumber(spRows[index].key);
    if(cur<0) return -1;
    // Compare against the last analysed song. Silent separators and
    // unanalysed tracks remain visible but do not interrupt the harmonic chain.
    // Preserve an explicit break marker even when unknown tracks intervene.
    int p=index-1, jump=0;
    bool hasBreak=false;
    while(p>=0) {
        const PlannerRow& prior=spRows[p];
        if(prior.type==2) return -1; // never bridge End of Playlist
        if(prior.type==1 && !hasBreak) {
            jump=prior.jump;
            hasBreak=true;
        }
        if(prior.type==0 && SpNumber(prior.key)>0) break;
        --p;
    }
    if(p<0) return -1;
    const int previous=SpNumber(spRows[p].key);
    const int goal=SpAdvance(previous,jump);
    const bool sameLetter=spRows[p].key.back()==spRows[index].key.back();
    // Explicit placeholders and automatic +2/+7/-2 transitions are valid.
    // Keep the letter (A/B) distinction, unless the explicit marker guides
    // the change, in which case its numeric target is the intention.
    if(jump!=0 && cur==goal && sameLetter) return 2;
    if(jump==0 && sameLetter) {
        if(cur==previous || cur==SpAdvance(previous,1) ||
           cur==SpAdvance(previous,-1) || cur==SpAdvance(previous,2) ||
           cur==SpAdvance(previous,7) || cur==SpAdvance(previous,-2))
            return 2;
    }
    if(jump==0 && cur==previous) return 2; // relative major/minor
    // Same numerical harmonic movement with a different A/B mode is
    // musically usable, but less secure: yellow rather than red.
    if(jump==0 && !sameLetter &&
       (cur==SpAdvance(previous,1) ||
        cur==SpAdvance(previous,-1) ||
        cur==SpAdvance(previous,2) ||
        cur==SpAdvance(previous,7) ||
        cur==SpAdvance(previous,-2))) return 1;
    const int distance=std::min((cur-goal+12)%12,(goal-cur+12)%12);
    return distance<=1 ? 1 : 0;
}
// A good harmonic transition with excessive tempo separation is yellow,
// not green. Red harmonic transitions remain red regardless of BPM.
// Compare measured tempos first, then half/double interpretations.
// Prefer the original whenever its gap is inside the soft compatibility
// window. Otherwise accept a musically equivalent half/double pulse.
int SpEffectiveTempoGap(int a,int b,int* adjustment=nullptr) {
    if(adjustment) *adjustment=0;
    if(a<=0 || b<=0) return 0;
    const int direct=abs(a-b);
    if(direct<=20) return direct;
    struct Match { int gap;int rank; };
    Match best{direct,0};
    const int aa[]={a,a/2,a*2};
    const int bb[]={b,b/2,b*2};
    // Check destination first: original, half, double; then source.
    const int order[][2]={{0,1},{0,2},{1,0},{2,0},
                          {1,1},{1,2},{2,1},{2,2}};
    for(int k=0;k<8;++k) {
        const int x=order[k][0],y=order[k][1];
        if(aa[x]<=0 || bb[y]<=0) continue;
        const int diff=abs(aa[x]-bb[y]);
        if(diff<=20 && (best.gap>20 || diff+3*(x+y)<best.gap+3*best.rank))
            best={diff,x+y};
    }
    if(adjustment) *adjustment=best.rank;
    return best.gap;
}
int SpQuality(int index) {
    const int harmonic=SpHarmonicQuality(index);
    if(harmonic!=2 || spRows[index].bpm<=0) return harmonic;
    // Follow the latest known BPM across intervening tracks without tempo
    // metadata; break and reserve markers remain section boundaries.
    const PlannerRow* previousKey=nullptr;
    const PlannerRow* previousTempo=nullptr;
    for(int p=index-1;p>=0;--p) {
        const PlannerRow& prior=spRows[p];
        if(prior.type==2 || prior.type==1) break;
        if(prior.type!=0) continue;
        if(!previousKey && SpNumber(prior.key)>0) previousKey=&prior;
        if(!previousTempo && prior.bpm>0) previousTempo=&prior;
        if(previousKey && previousTempo) break;
    }
    // Half/double tempo compatibility is useful for sorting, but the
    // original BPM values still describe an audible rhythmic risk.
    // Warn in yellow whenever a correction was actually needed, even +7.
    if(previousTempo) {
        int adjustment=0;
        const int difference=SpEffectiveTempoGap(
            previousTempo->bpm,spRows[index].bpm,&adjustment);
        if(adjustment>0) return 1;
        // +7 is an intentional energy change: exempt a direct wide
        // tempo jump, but never conceal an inferred half/double tempo.
        if(previousKey && !previousKey->key.empty() &&
           SpNumber(spRows[index].key)==SpAdvance(
               SpNumber(previousKey->key),7))
            return harmonic;
        if(difference>20) return 1;
    }
    return harmonic;
}
std::wstring SpBreakStatus(int index) {
    if(index<0 || index>=(int)spRows.size() || spRows[index].type!=1)
        return L"";
    int prior=index-1;
    while(prior>=0 && spRows[prior].type==0 &&
          SpNumber(spRows[prior].key)<0) --prior;
    int next=index+1;
    while(next<(int)spRows.size() && spRows[next].type==0 &&
          SpNumber(spRows[next].key)<0) ++next;
    if(prior<0 || next>=(int)spRows.size() ||
       spRows[prior].type!=0 || spRows[next].type!=0 ||
       SpNumber(spRows[prior].key)<0 || SpNumber(spRows[next].key)<0)
        return L"BREAK --";
    return spRows[prior].key.back()==spRows[next].key.back() &&
           SpNumber(spRows[next].key)==SpAdvance(
             SpNumber(spRows[prior].key),spRows[index].jump)
           ? L"BREAK OK" : L"BREAK !";
}
void SpUpdateTrackNumber();
void SpRender(int focus=-1,bool followMove=false) {
    if(!spList || !IsWindow(spList)) return;
    // ListView_DeleteAllItems resets its scroll position. Remember the
    // visible region before rebuilding so edits do not jump the viewport.
    if(spRendering) return;
    spRendering=true;
    const int previousTop=ListView_GetTopIndex(spList);
    RECT itemRect{};
    int rowHeight=0;
    if(ListView_GetItemCount(spList)>0 &&
       ListView_GetItemRect(spList,0,&itemRect,LVIR_BOUNDS))
        rowHeight=itemRect.bottom-itemRect.top;
    if(rowHeight<=0) rowHeight=20;
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
        const bool inReserve=SpInReserve(i);
        const int quality=row.type==0 && !inReserve?SpQuality(i):-1;
        std::wstring status=row.type==2?L"RESERVE":row.type==1?SpBreakStatus(i):
          inReserve?L"UNSORTED":quality==2?L"GREEN":
          quality==1?L"YELLOW":quality==0?L"RED":L"--";
        ListView_SetItemText(spList,i,4,const_cast<wchar_t*>(status.c_str()));
        std::wstring lock=row.locked?L"LOCKED":L"";
        ListView_SetItemText(spList,i,5,const_cast<wchar_t*>(lock.c_str()));
        ++i;
    }
    if(focus>=0 && focus<(int)spRows.size())
        ListView_SetItemState(spList,focus,LVIS_SELECTED|LVIS_FOCUSED,
                             LVIS_SELECTED|LVIS_FOCUSED);
    // Restore the viewport after setting selection. Selecting items can
    // itself scroll the control, especially near the bottom of the window.
    // Re-enable painting before scrolling: LVM_SCROLL while redraw is
    // disabled can leave stale row fragments in classic Windows ListViews.
    SendMessageW(spList,WM_SETREDRAW,TRUE,0);
    const int currentTop=ListView_GetTopIndex(spList);
    if(previousTop!=currentTop)
        ListView_Scroll(spList,0,(previousTop-currentTop)*rowHeight);
    // Only Up/Down or a drag should follow the moved row, and even then
    // scrolling is needed solely when the destination leaves the viewport.
    if(followMove && focus>=0 && focus<(int)spRows.size()) {
        const int first=ListView_GetTopIndex(spList);
        const int visible=std::max(1,ListView_GetCountPerPage(spList));
        if(focus<first || focus>=first+visible)
            ListView_EnsureVisible(spList,focus,FALSE);
    }
    // Force a complete repaint after the reorder rather than relying on
    // Win32's partial scroll optimizations, which left visual artifacts.
    RedrawWindow(spList,nullptr,nullptr,
        RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);
    spRendering=false;
    SpUpdateTrackNumber();
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
    // Import preserves the Playlist Editor order, including silent separators
    // and tracks without analysis. Reserve is populated only by Sort All.
    SpRender();
}
// Live position is read from the current Winamp playlist, never the Planner order.
// Read-only: no modal dialogs, playlist mutations, or synthesized input.
int SpCurrentWinampPosition(const PlannerRow& row) {
    if(row.type!=0 || row.path.empty() || !g_winamp || !IsWindow(g_winamp)) return -1;
    const int count=std::max(0,(int)SendMessageW(g_winamp,WM_WA_IPC,0,IPC_GETLISTLENGTH));
    // Original occurrence wins if still present; otherwise find the path afresh.
    if(row.originalIndex>=0 && row.originalIndex<count &&
       _wcsicmp(PlaylistPath(row.originalIndex).c_str(),row.path.c_str())==0)
        return row.originalIndex;
    for(int i=0;i<count;++i)
        if(_wcsicmp(PlaylistPath(i).c_str(),row.path.c_str())==0) return i;
    return -1;
}
void SpUpdateTrackNumber() {
    if(spRendering || !spTrackNumber || !IsWindow(spTrackNumber)) return;
    int selected=spList?ListView_GetNextItem(spList,-1,LVNI_FOCUSED):-1;
    if(selected<0 || selected>=(int)spRows.size() ||
       !(ListView_GetItemState(spList,selected,LVIS_SELECTED)&LVIS_SELECTED))
        selected=SpSelectedFirst();
    std::wstring value=L"--";
    if(selected>=0 && selected<(int)spRows.size()) {
        const int i=SpCurrentWinampPosition(spRows[selected]);
        if(i>=0) value=std::to_wstring(i+1);
    }
    wchar_t previous[40]{};
    GetWindowTextW(spTrackNumber,previous,40);
    if(value!=previous) SetWindowTextW(spTrackNumber,value.c_str());
}
// Move only through unlocked slots. Locked tracks remain at their exact
// indices while other tracks can travel across them, with either control.
int SpNextUnlocked(int from,int direction) {
    for(int i=from+direction;i>=0 && i<(int)spRows.size();i+=direction)
        if(!spRows[i].locked) return i;
    return -1;
}
bool SpMoveAcrossLocks(int source,int target) {
    if(source<0 || source>=(int)spRows.size() ||
       target<0 || target>=(int)spRows.size() ||
       source==target || spRows[source].locked) return false;
    const int direction=target>source?1:-1;
    // When dropped directly over a locked song, go to the first movable
    // slot on the opposite side of the lock, in the drag direction.
    if(spRows[target].locked) {
        int candidate=target;
        while(candidate>=0 && candidate<(int)spRows.size() &&
              spRows[candidate].locked) candidate+=direction;
        if(candidate<0 || candidate>=(int)spRows.size()) return false;
        target=candidate;
    }
    if(source==target) return false;
    SpSnapshot();
    int current=source;
    while(current!=target) {
        const int next=SpNextUnlocked(current,direction);
        if(next<0 || (direction>0 && next>target) ||
                     (direction<0 && next<target)) break;
        std::swap(spRows[current],spRows[next]);
        current=next;
    }
    SpRender(current,true);
    return true;
}
void SpMove(int direction) {
    if(!spList || spRows.empty()) return;
    const int n=(int)spRows.size();
    std::vector<unsigned char> selected(n,0);
    int count=0;
    for(int i=-1;(i=ListView_GetNextItem(spList,i,LVNI_SELECTED))!=-1;) {
        if(i>=0 && i<n) {selected[i]=1;++count;}
    }
    if(!count) return;
    // A single click keeps the familiar behavior. For multi-selection, move
    // all selected unlocked rows one available slot, preserving their order.
    // Locked rows never move, even if they are selected.
    if(count==1) {
        const int source=SpSelectedFirst();
        if(source<0 || spRows[source].locked) return;
        const int target=SpNextUnlocked(source,direction);
        if(target>=0) SpMoveAcrossLocks(source,target);
        return;
    }
    std::vector<int> available;
    for(int i=0;i<n;++i) if(!spRows[i].locked) available.push_back(i);
    bool canMove=false;
    if(direction<0) {
        for(int k=1;k<(int)available.size();++k)
            if(selected[available[k]] && !selected[available[k-1]]) {canMove=true;break;}
    } else {
        for(int k=0;k+1<(int)available.size();++k)
            if(selected[available[k]] && !selected[available[k+1]]) {canMove=true;break;}
    }
    if(!canMove) return;
    SpSnapshot();
    if(direction<0) {
        for(int k=1;k<(int)available.size();++k) {
            const int from=available[k],to=available[k-1];
            if(selected[from] && !selected[to]) {
                std::swap(spRows[from],spRows[to]);
                std::swap(selected[from],selected[to]);
            }
        }
    } else {
        for(int k=(int)available.size()-2;k>=0;--k) {
            const int from=available[k],to=available[k+1];
            if(selected[from] && !selected[to]) {
                std::swap(spRows[from],spRows[to]);
                std::swap(selected[from],selected[to]);
            }
        }
    }
    SpRender();
    int first=-1;
    for(int i=0;i<n;++i) if(selected[i]) {
        ListView_SetItemState(spList,i,LVIS_SELECTED,LVIS_SELECTED);
        if(first<0) first=i;
    }
    if(first>=0) {
        ListView_SetItemState(spList,first,LVIS_FOCUSED,LVIS_FOCUSED);
        ListView_EnsureVisible(spList,first,FALSE);
    }
    SpUpdateTrackNumber();
}
void SpRemoveSelected() {
    if(!spList || spRows.empty()) return;
    std::vector<int> selected;
    for(int i=-1;(i=ListView_GetNextItem(spList,i,LVNI_SELECTED))!=-1;)
        if(i>=0 && i<(int)spRows.size()) selected.push_back(i);
    if(selected.empty()) return;
    const int focus=selected.front();
    SpSnapshot();
    // Remove in reverse so the indices remain valid. A lock protects
    // sorting and dragging; an explicit Remove is allowed.
    for(auto it=selected.rbegin();it!=selected.rend();++it)
        spRows.erase(spRows.begin()+*it);
    SpRender(spRows.empty()?-1:std::min(focus,(int)spRows.size()-1));
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
// Beam-search harmonic optimizer. This is entirely in-memory; it never
// modifies Winamp's playlist or audio playback.
int SpTransitionCost(const PlannerRow* from,const PlannerRow& to) {
    if(!from || SpNumber(from->key)<0 || SpNumber(to.key)<0) return 0;
    const int prior=SpNumber(from->key), next=SpNumber(to.key);
    const bool sameLetter=from->key.back()==to.key.back();
    // Priority: same key, relative A/B, +1, +2, +7, -2, then imperfect transitions.
    if(sameLetter && next==prior) return 0;
    if(next==prior) return 6; // relative minor/major: 10A <-> 10B
    if(sameLetter && next==SpAdvance(prior,1)) return 12;
    if(sameLetter && next==SpAdvance(prior,2)) return 26;
    if(sameLetter && next==SpAdvance(prior,7)) return 40;
    if(sameLetter && next==SpAdvance(prior,-2)) return 54;
    // A/B mismatch on an otherwise useful harmonic move is valid,
    // but carries a higher cost than the same-letter equivalent.
    if(!sameLetter && next==SpAdvance(prior,1)) return 72;
    if(!sameLetter && next==SpAdvance(prior,2)) return 86;
    if(!sameLetter && next==SpAdvance(prior,7)) return 92;
    if(!sameLetter && next==SpAdvance(prior,-2)) return 104;
    if(!sameLetter && next==SpAdvance(prior,-1)) return 116;
    return 145;
}
constexpr int SP_BPM_SOFT_LIMIT=15;
constexpr int SP_BPM_WARNING_LIMIT=20;
int SpTempoCost(const PlannerRow* from,const PlannerRow& to,
                const PlannerRow* harmonicFrom=nullptr) {
    if(!from || from->bpm<=0 || to.bpm<=0) return 0;
    int adjustment=0;
    const int difference=SpEffectiveTempoGap(from->bpm,to.bpm,&adjustment);
    const PlannerRow* keyFrom=harmonicFrom?harmonicFrom:from;
    // Keep the existing +7 energy-break exception if even the best
    // half/double interpretation remains wide.
    if(SpNumber(keyFrom->key)>0 && SpNumber(to.key)>0 &&
       SpNumber(to.key)==SpAdvance(SpNumber(keyFrom->key),7))
        return (difference<=SP_BPM_WARNING_LIMIT?difference/5:5)+adjustment*3;
    if(difference<=SP_BPM_SOFT_LIMIT) return difference/5+adjustment*3;
    if(difference<=SP_BPM_WARNING_LIMIT)
        return 3+(difference-SP_BPM_SOFT_LIMIT)*2+adjustment*3;
    return 13+std::min(160,(difference-SP_BPM_WARNING_LIMIT)*4);
}
// Fixed boundaries are never crossed. Unanalysed tracks can be skipped
// when determining the harmonic reference, as in SpQuality.
const PlannerRow* SpNeighbourKey(int from,int step) {
    for(int i=from;i>=0 && i<(int)spRows.size();i+=step) {
        const auto& row=spRows[i];
        if(row.type==2 || row.type==1) return nullptr;
        if(row.type==0 && SpNumber(row.key)>0) return &row;
    }
    return nullptr;
}
const PlannerRow* SpNeighbourBpm(int from,int step) {
    for(int i=from;i>=0 && i<(int)spRows.size();i+=step) {
        const auto& row=spRows[i];
        if(row.type==2 || row.type==1) return nullptr;
        if(row.type==0 && row.bpm>0) return &row;
    }
    return nullptr;
}
// Explicit break markers are directives, not weights in the scoring model.
// For the first analysed song following a marker, the Camelot number and
// A/B letter must match exactly the last analysed song + the marked jump.
struct SpBreakConstraint {
    const PlannerRow* anchor=nullptr;
    int jump=0;
};
SpBreakConstraint SpBreakBeforeRun(int firstSlot) {
    int i=firstSlot-1;
    while(i>=0 && spRows[i].type==0 && SpNumber(spRows[i].key)<0) --i;
    if(i<0 || spRows[i].type!=1) return {};
    const int shift=spRows[i].jump;
    const PlannerRow* anchor=SpNeighbourKey(i-1,-1);
    return {anchor,shift};
}
bool SpBreakMatches(const PlannerRow& before,const PlannerRow& next,int shift) {
    const int a=SpNumber(before.key),b=SpNumber(next.key);
    return a>0 && b>0 && before.key.back()==next.key.back() &&
           b==SpAdvance(a,shift);
}
// Goofy is a separate planning preference. It never changes the
// harmonic-color evaluation, BPM matching, or manual break rules.
int SpGoofyTransitionCost(int previous,int current,int repetitions) {
    if(previous<1 || current<1) return 0;
    if(previous==current) {
        if(repetitions>=3) return 1200; // never four unless unavoidable
        if(repetitions==2) return 240;  // strongly discourage a third
        return 18; // prefer +1 slightly over an identical number
    }
    if(repetitions>=2 && (current==SpAdvance(previous,2) ||
                          current==SpAdvance(previous,7)))
        return -68; // force +2/+7 ahead of +1 after two repeats
    if(current==SpAdvance(previous,1)) return -12;
    return 0;
}
int SpPriorRepeatCount(int firstSlot,int number) {
    if(number<1) return 0;
    int repeated=0;
    for(int i=firstSlot-1;i>=0;--i) {
        const auto& row=spRows[i];
        if(row.type!=0) break;
        const int k=SpNumber(row.key);
        if(k<1) continue;
        if(k!=number) break;
        ++repeated;
    }
    return repeated;
}
struct SpBeamNode {
    std::vector<int> path;
    std::vector<unsigned char> used;
    int score=0;
    int orderScore=0;
    int last=-1;
    int lastNumber=-1;
    int repeatCount=0;
};
std::vector<PlannerRow> SpBeamOrder(const std::vector<PlannerRow>& pool,
                                    const PlannerRow* before,
                                    const PlannerRow* after,
                                    const PlannerRow* beforeTempo,
                                    SpBreakConstraint forced,
                                    bool goofy,int initialRepetitions) {
    const int n=(int)pool.size();
    if(n<=1) return pool;
    // A hard break is enforced only if the selected run actually contains
    // a matching key. If no such song exists, keep the segment as it was;
    // the marker remains visibly unsatisfied rather than silently weakened.
    bool hasMatch=false;
    if(forced.anchor) {
        for(const auto& song:pool)
            if(SpBreakMatches(*forced.anchor,song,forced.jump)) {
                hasMatch=true;break;
            }
        if(!hasMatch) return pool;
    }
    constexpr size_t width=96;
    std::vector<SpBeamNode> beam(1);
    beam[0].used.assign(n,0);
    if(before) beam[0].lastNumber=SpNumber(before->key);
    if(goofy) beam[0].repeatCount=initialRepetitions;
    for(int depth=0;depth<n;++depth) {
        std::vector<SpBeamNode> next;
        next.reserve(beam.size()*n);
        for(const auto& node:beam) {
            const PlannerRow* previous=node.last>=0?&pool[node.last]:before;
            // Harmonic and tempo references can come from different tracks.
            // A song with known key but no BPM must not hide the last
            // measured tempo (e.g. 78 -> unknown -> 123).
            const PlannerRow* tempoPrevious=beforeTempo;
            for(int k=(int)node.path.size()-1;k>=0;--k) {
                const PlannerRow& past=pool[node.path[k]];
                if(past.bpm>0) {tempoPrevious=&past;break;}
            }
            bool hasKnown=false;
            for(int j=0;j<n;++j)
                if(!node.used[j] && SpNumber(pool[j].key)>0) {
                    hasKnown=true;break;
                }
            for(int j=0;j<n;++j) {
                if(node.used[j]) continue;
                // Hard constraint on the FIRST analysed track after a marker.
                // Silent tracks are transparent to harmonic comparisons.
                if(forced.anchor && node.path.empty() && hasMatch &&
                   !SpBreakMatches(*forced.anchor,pool[j],forced.jump))
                    continue;
                // A fourth matching Camelot number is forbidden whenever
                // another analysed number remains in this movable section.
                // A/B are treated as the same number for this rule.
                if(goofy && !(forced.anchor && node.path.empty()) &&
                   node.repeatCount>=3 &&
                   SpNumber(pool[j].key)==node.lastNumber) {
                    bool alternative=false;
                    for(int t=0;t<n;++t)
                        if(!node.used[t] && t!=j &&
                           SpNumber(pool[t].key)>0 &&
                           SpNumber(pool[t].key)!=node.lastNumber) {
                            alternative=true;break;
                        }
                    if(alternative) continue;
                }
                // Keep unknown/silent songs at the end of the movable run.
                if(hasKnown && SpNumber(pool[j].key)<0) continue;
                SpBeamNode candidate=node;
                candidate.used[j]=1;
                candidate.path.push_back(j);
                candidate.last=j;
                const int number=SpNumber(pool[j].key);
                const int playful=goofy ? SpGoofyTransitionCost(node.lastNumber,number,node.repeatCount) : 0;
                if(number>0) {
                    candidate.repeatCount=(number==node.lastNumber)?node.repeatCount+1:1;
                    candidate.lastNumber=number;
                }
                const int cost=SpTransitionCost(previous,pool[j])+
                               SpTempoCost(tempoPrevious,pool[j],previous);
                candidate.score+=cost+playful;
                // Stable order for equally good sequences.
                candidate.orderScore+=abs(j-depth);
                // On the final track, evaluate the fixed exit track as well.
                if(depth==n-1 && after) {
                    // Ignore silent/unanalysed trailers for the endpoint,
                    // just as the Planner's transition indicators do.
                    const PlannerRow* exitFrom=nullptr;
                    for(int k=(int)candidate.path.size()-1;k>=0;--k) {
                        const PlannerRow& tail=pool[candidate.path[k]];
                        if(SpNumber(tail.key)>0) {exitFrom=&tail;break;}
                    }
                    if(exitFrom) {
                        candidate.score+=SpTransitionCost(exitFrom,*after)*2+
                                         SpTempoCost(exitFrom,*after);
                    }
                }
                next.push_back(std::move(candidate));
            }
        }
        // Prefer total musical cost, with original position as a final tie.
        std::stable_sort(next.begin(),next.end(),[](const SpBeamNode& a,
                                                     const SpBeamNode& b) {
            if(a.score!=b.score) return a.score<b.score;
            return a.orderScore<b.orderScore;
        });
        if(next.size()>width) next.resize(width);
        beam=std::move(next);
        if(beam.empty()) return pool;
    }
    std::vector<PlannerRow> result;
    result.reserve(n);
    for(int id:beam.front().path) result.push_back(pool[id]);
    return result;
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
    // Locked rows, breaks, and End of Playlist are hard boundaries.
    // No existing locked position or manual break can move during sorting.
    int start=0,reference=1;
    while(start<(int)spRows.size()) {
        if(spRows[start].type==2) break; // reserve is not sorted
        if(spRows[start].type==1) {
            reference=SpAdvance(reference,spRows[start].jump);
            ++start;continue;
        }
        if(spRows[start].locked) {
            if(SpNumber(spRows[start].key)>0)
                reference=SpNumber(spRows[start].key);
            ++start;continue;
        }
        int finish=start;
        while(finish<(int)spRows.size() && spRows[finish].type==0 &&
              !spRows[finish].locked) ++finish;
        std::vector<int> slots;
        std::vector<PlannerRow> pool;
        for(int i=start;i<finish;++i) if(selection[i]) {
            slots.push_back(i);pool.push_back(spRows[i]);
        }
        // Each contiguous selection is optimized separately. Unselected
        // tracks within the same unlocked section stay fixed and become
        // possible entry/exit anchors for adjacent selected runs.
        int cursor=0;
        while(cursor<(int)slots.size()) {
            int last=cursor+1;
            while(last<(int)slots.size() && slots[last]==slots[last-1]+1)
                ++last;
            const int firstSlot=slots[cursor], lastSlot=slots[last-1];
            std::vector<PlannerRow> run;
            run.reserve(last-cursor);
            for(int j=cursor;j<last;++j) run.push_back(spRows[slots[j]]);
            const PlannerRow* before=SpNeighbourKey(firstSlot-1,-1);
            // Exit is optional. An immediately following unknown/silent
            // track or the end of the playlist is NOT a target to chase.
            // Unlike the entry lookup, do not skip over unknown tracks to
            // impose a distant harmonic endpoint.
            const PlannerRow* after=nullptr;
            if(lastSlot+1<(int)spRows.size()) {
                const PlannerRow& next=spRows[lastSlot+1];
                if(next.type==0 && SpNumber(next.key)>0)
                    after=&next;
            }
            const PlannerRow* beforeTempo=SpNeighbourBpm(firstSlot-1,-1);
            const SpBreakConstraint forced=SpBreakBeforeRun(firstSlot);
            // The marker's predecessor is the real harmonic starting point,
            // even though the marker separates the two sorted sections.
            if(forced.anchor) before=forced.anchor;
            const int repetitions=spGoofy && before ? SpPriorRepeatCount(firstSlot,SpNumber(before->key)) : 0;
            const auto optimized=SpBeamOrder(run,before,after,beforeTempo,forced,spGoofy,repetitions);
            for(int j=cursor;j<last;++j)
                spRows[slots[j]]=optimized[j-cursor];
            cursor=last;
        }
        for(int i=start;i<finish;++i)
            if(SpNumber(spRows[i].key)>0)
                reference=SpNumber(spRows[i].key);
        start=finish;
    }
    if(!selectionOnly) {
        // When the final active section has no fixed anchors after it,
        // unscanned tracks at its tail become the reserve. This cannot shift
        // any locked row or manually positioned transition.
        int end=(int)spRows.size();
        for(int i=0;i<(int)spRows.size();++i)
            if(spRows[i].type==2) {end=i;break;}
        int fixed=-1;
        for(int i=0;i<end;++i)
            if(spRows[i].type!=0 || spRows[i].locked) fixed=i;
        int firstUnknown=end;
        while(firstUnknown>fixed+1 && firstUnknown>0 &&
              spRows[firstUnknown-1].type==0 &&
              SpNumber(spRows[firstUnknown-1].key)<0 &&
              !spRows[firstUnknown-1].locked)
            --firstUnknown;
        if(firstUnknown<end) {
            if(end==(int)spRows.size()) {
                PlannerRow divider;divider.type=2;
                spRows.insert(spRows.begin()+firstUnknown,divider);
            } else if(!spRows[end].locked) {
                // Move an existing unlocked divider upward, preserving all
                // song order within both the reserve and active section.
                PlannerRow divider=std::move(spRows[end]);
                spRows.erase(spRows.begin()+end);
                spRows.insert(spRows.begin()+firstUnknown,std::move(divider));
            }
        }
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
void SpExportToWinamp(HWND hwnd) {
    if(!g_winamp || !IsWindow(g_winamp) || spRows.empty()) return;
    std::vector<std::wstring> paths;
    for(const auto& row:spRows) {
        if(row.type!=0) continue; // markers never become playlist entries
        if(row.path.empty()) {
            MessageBoxW(hwnd,L"Export cancelled: a track has no file path.",
                        L"WaveCue - Export",MB_OK|MB_ICONWARNING);
            return;
        }
        // Stream URLs are also valid playlist entries.
        const bool url=row.path.find(L"://")!=std::wstring::npos;
        if(!url && GetFileAttributesW(row.path.c_str())==INVALID_FILE_ATTRIBUTES) {
            std::wstring error=L"Export cancelled: missing file:\n"+row.path;
            MessageBoxW(hwnd,error.c_str(),L"WaveCue - Export",
                        MB_OK|MB_ICONWARNING);
            return;
        }
        paths.push_back(row.path);
    }
    if(paths.empty()) return;
    std::wstring prompt=L"Replace the current Winamp playlist with all "+
        std::to_wstring(paths.size())+
        L" Planner tracks (including the reserve)?\n\n"
        L"The current playlist will be cleared. This cannot be undone.";
    if(MessageBoxW(hwnd,prompt.c_str(),L"Export to Winamp",
                   MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return;
    // These are Winamp's native playlist operations, not synthesized input.
    SendMessageW(g_winamp,WM_WA_IPC,0,101 /* IPC_DELETE */);
    struct EnqueueFileW { const wchar_t* filename;const wchar_t* title;int length; };
    for(const auto& path:paths) {
        EnqueueFileW entry{path.c_str(),nullptr,-1};
        SendMessageW(g_winamp,WM_WA_IPC,(WPARAM)&entry,
                     1100 /* IPC_ENQUEUEFILEW */);
    }
    const int count=(int)SendMessageW(g_winamp,WM_WA_IPC,0,IPC_GETLISTLENGTH);
    if(count!=(int)paths.size()) {
        MessageBoxW(hwnd,L"Export completed with a different number of "
                        L"tracks than expected. Please inspect the Playlist Editor.",
                    L"WaveCue - Export",MB_OK|MB_ICONWARNING);
    }
    SpUpdateTrackNumber();
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
        ListView_SetBkColor(spList,RGB(0,0,0));
        ListView_SetTextBkColor(spList,RGB(0,0,0));
        ListView_SetTextColor(spList,RGB(199,213,200));
        SetWindowSubclass(spList,SpListKeyboard,1,0);
        const wchar_t* labels[]={L"Track",L"#",L"BPM",L"Camelot",L"Transition",L"Lock"};
        const int widths[]={495,43,65,83,120,90};
        for(int j=0;j<6;++j) {
            LVCOLUMNW c{}; c.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
            c.pszText=const_cast<wchar_t*>(labels[j]);c.cx=widths[j];c.iSubItem=j;
            ListView_InsertColumn(spList,j,&c);
        }
        // Match WaveCue's existing UI typeface and size, changing weight only.
        HFONT baseFont=g_font?g_font:(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        LOGFONTW boldStyle{};
        if(GetObjectW(baseFont,sizeof(boldStyle),&boldStyle)==sizeof(boldStyle)) {
            boldStyle.lfWeight=FW_BOLD;
            spBoldFont=CreateFontIndirectW(&boldStyle);
        }
        SendMessageW(spList,WM_SETFONT,(WPARAM)(spBoldFont?spBoldFont:baseFont),TRUE);
        SpAddButton(hwnd,SP_IMPORT,L"Import / Refresh",12,10,124);
        SpAddButton(hwnd,SP_SORT_SEL,L"Sort Selection",145,10,122);
        SpAddButton(hwnd,SP_SORT,L"Sort All",276,10,86);
        SpAddButton(hwnd,SP_UNDO,L"Undo",371,10,70);
        HWND goofy=CreateWindowExW(0,L"BUTTON",L"Goofy",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
            641,12,95,23,hwnd,(HMENU)(INT_PTR)SP_GOOFY,g_plugin.hDllInstance,nullptr);
        if(goofy) {if(g_font) SendMessageW(goofy,WM_SETFONT,(WPARAM)g_font,TRUE);
            SendMessageW(goofy,BM_SETCHECK,spGoofy?BST_CHECKED:BST_UNCHECKED,0);}
        SpAddButton(hwnd,SP_EXPORT,L"Export to Winamp",764,10,168);
        HWND numberLabel=CreateWindowExW(0,L"STATIC",L"WINAMP #",
            WS_CHILD|WS_VISIBLE|SS_LEFT,465,15,95,21,hwnd,nullptr,
            g_plugin.hDllInstance,nullptr);
        if(numberLabel&&g_font) SendMessageW(numberLabel,WM_SETFONT,(WPARAM)g_font,TRUE);
        spTrackNumber=CreateWindowExW(0,L"STATIC",L"--",
            WS_CHILD|WS_VISIBLE|SS_CENTER|SS_SUNKEN,563,9,64,28,hwnd,
            nullptr,g_plugin.hDllInstance,nullptr);
        if(spTrackNumber&&g_bigFont)
            SendMessageW(spTrackNumber,WM_SETFONT,(WPARAM)g_bigFont,TRUE);
        SetTimer(hwnd,SP_NUMBER_TIMER,500,nullptr);
        SpAddButton(hwnd,SP_UP,L"Up",12,545,60);
        SpAddButton(hwnd,SP_DOWN,L"Down",78,545,60);
        SpAddButton(hwnd,SP_REMOVE,L"Remove",146,545,106);
        SpAddButton(hwnd,SP_PLUS2,L"+2",263,545,58);
        SpAddButton(hwnd,SP_MINUS2,L"-2",326,545,58);
        SpAddButton(hwnd,SP_PLUS7,L"+7",389,545,58);
        SpAddButton(hwnd,SP_MINUS7,L"-7",452,545,58);
        SpAddButton(hwnd,SP_END,L"End of Playlist",520,545,137);
        HWND hint=CreateWindowExW(0,L"STATIC",
          L"Select a track for its Winamp number. Double-click or click Lock to lock. Remove only edits the Planner.",
          WS_CHILD|WS_VISIBLE,12,584,905,23,hwnd,nullptr,g_plugin.hDllInstance,nullptr);
        if(hint&&g_font)SendMessageW(hint,WM_SETFONT,(WPARAM)g_font,TRUE);
        return 0;
    }
    case WM_TIMER:
        if(wp==SP_NUMBER_TIMER) {SpUpdateTrackNumber();return 0;}
        break;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case SP_IMPORT: SpImport();break;
        case SP_GOOFY:spGoofy=SendMessageW((HWND)lp,BM_GETCHECK,0,0)==BST_CHECKED;break;
        case SP_EXPORT: SpExportToWinamp(hwnd);break;
        case SP_SORT: SpSort(false);break;
        case SP_SORT_SEL: SpSort(true);break;
        case SP_UP: SpMove(-1);break;
        case SP_DOWN: SpMove(1);break;
        case SP_REMOVE: SpRemoveSelected();break;
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
            if(target>=0 && source!=target)
                SpMoveAcrossLocks(source,target);
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* hdr=reinterpret_cast<NMHDR*>(lp);
        if(hdr && hdr->hwndFrom==spList) {
            if(!spRendering && (hdr->code==LVN_ITEMCHANGED || hdr->code==NM_CLICK)) {
                SpUpdateTrackNumber();
            }
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
                        draw->clrTextBk=RGB(0,0,0);
                        // User-selected Win32 color dialog HSL palette (scale 0..240):
                        // Green H80 S240 L103, Red H0 S237 L103,
                        // Yellow H40 S240 L120, Locked H160 S240 L120.
                        draw->clrText=spRows[i].locked?RGB(0,0,255):
                            q==2?RGB(0,219,0):q==1?RGB(255,255,0):
                            q==0?RGB(218,2,2):RGB(199,213,200);
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
    case WM_CLOSE:spDragFrom=-1; if(GetCapture()==hwnd)ReleaseCapture();ShowWindow(hwnd,SW_HIDE);return 0;
    case WM_DESTROY:
        KillTimer(hwnd,SP_NUMBER_TIMER);
        // Detach the owned font before destroying it.
        if(spList && IsWindow(spList) && spBoldFont)
            SendMessageW(spList,WM_SETFONT,
                (WPARAM)(g_font?g_font:GetStockObject(DEFAULT_GUI_FONT)),FALSE);
        if(spBoldFont) {DeleteObject(spBoldFont);spBoldFont=nullptr;}
        spTrackNumber=nullptr;spList=nullptr;spWindow=nullptr;return 0;
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
        SpUpdateTrackNumber();
    }
}
