// deck
//
interface ActionProps {
    /** 功能uuid */
    uuid: string;
    /** 功能实例uuid */
    actionid: string;
    /** 上位机按钮key */
    key: string;
}

type Cmd = "run" | "add" | "clear" | "paramfromapp" | "paramfromplugin" | "setactive" | "state" | "openurl" | "openview" | "selectdialog" | "logMessage" | "hotkey" | "showAlert" | "sendToPropertyInspector" | "sendToPlugin" | "getSettings" | "setSettings" | "didReceiveSettings" | "setGlobalSettings" | "didReceiveGlobalSettings" | "getGlobalSettings" | "keydown" | "keyup" | "dialdown" | "dialup" | "dialrotate";
interface DeckRespDataBase<TCmd extends Cmd> {
    cmd: TCmd;
    /** 0-成功，非0-失败 */
    code: number;
}

interface ParamRespData<TCmd extends Cmd> extends DeckRespDataBase<TCmd>, ActionProps {
    param?: Record<string, any> | null;
}

interface SetActiveRespData extends ParamRespData<'setactive'> {
    active: boolean;
}

interface StateRespData extends DeckRespDataBase<'state'> {
    cmdType: 'NOTIFY';
    param?: Record<string, any> | null;
}

interface OpenUrlRespData extends DeckRespDataBase<'openurl'> {
    cmdType: 'NOTIFY';
    url: string;
    /** false-远端，true-本地 */
    local?: boolean;
}

interface SelectDialogRespData extends DeckRespDataBase<'selectdialog'> {
    cmdType: 'REQUEST';
    type: 'file' | 'folder';
    filter: string;
    path: string;
}


// api
//
type UlanzideckApiData<TData> = TData & {
    /** 唯一id */
    context: string;
}

type UlanzideckParamRespData<TCmd extends Cmd> = Readonly<UlanzideckApiData<ParamRespData<TCmd>>>;
type UlanzideckClearRespData = Readonly<DeckRespDataBase<'clear'> & { param?: UlanzideckApiData<ActionProps>[] | null }>;
type UlanzideckSetActiveRespData = Readonly<UlanzideckApiData<SetActiveRespData>>;
type UlanzideckStateRespData = Readonly<UlanzideckApiData<StateRespData>>;
type UlanzideckOpenUrlRespData = Readonly<UlanzideckApiData<OpenUrlRespData>>;
type UlanzideckSelectDialogRespData = Readonly<UlanzideckApiData<SelectDialogRespData>>;


// events
// 
export type OnConnected = (data: never) => void;
export type OnClose = () => void;
export type OnError = (error: string) => void;

export type OnCmdAddResp = (data: UlanzideckParamRespData<"add">) => void;
export type OnCmdParamFromAppResp = (data: UlanzideckParamRespData<"paramfromapp">) => void;
export type OnCmdParamFromPluginResp = (data: UlanzideckParamRespData<"paramfromplugin">) => void;

export type OnCmdRunResp = (data: UlanzideckParamRespData<'run'>) => void;
export type OnCmdClearResp = (data: UlanzideckClearRespData) => void;
export type OnCmdSetActiveResp = (data: UlanzideckSetActiveRespData) => void;
export type OnCmdStateResp = (data: UlanzideckStateRespData) => void;
export type OnCmdSelectDialogResp = (data: UlanzideckSelectDialogRespData) => void;